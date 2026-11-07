#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""One-command Docker lifecycle around the existing native bulk importer."""
from __future__ import annotations
import argparse
import csv
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import math
import hashlib
import os
from pathlib import Path
import secrets
import shlex
import signal
import sqlite3
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.ingestion.docker_environment import DockerEnvironment, WorkflowError, WorkflowInterrupted, private_json


@contextmanager
def cancellation_scope():
    previous = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
    def interrupted(signum, frame):
        # Once cancellation begins, subsequent signals must not interrupt child termination,
        # journal recording or ownership-checked cleanup. SIGKILL remains available.
        for sig in previous:
            signal.signal(sig, signal.SIG_IGN)
        raise WorkflowInterrupted(f'Command interrupted by signal {signum}')
    try:
        for sig in previous:
            signal.signal(sig, interrupted)
        yield
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def tail(path):
    with Path(path).open('rb') as f:
        f.seek(max(0, Path(path).stat().st_size - 8192))
        return f.read().decode('utf-8', errors='replace').strip()


def stage(environment, name, argv):
    print(f'load: {name}; logs: {environment.output / "logs" / name}', file=sys.stderr, flush=True)
    environment.state['phase'] = name; environment.save()
    stem = name
    if any((environment.output / 'logs' / f'{name}.{suffix}.log').exists() for suffix in ('stdout', 'stderr')):
        stem += '-' + str(time.time_ns())
    stdout, stderr = [environment.output / 'logs' / f'{stem}.{suffix}.log' for suffix in ('stdout', 'stderr')]
    with os.fdopen(os.open(stdout, os.O_CREAT | os.O_WRONLY | os.O_EXCL, 0o600), 'w') as out, \
         os.fdopen(os.open(stderr, os.O_CREAT | os.O_WRONLY | os.O_EXCL, 0o600), 'w') as err:
        with subprocess.Popen(list(map(str, argv)), env=environment.env, stdout=out, stderr=err, start_new_session=True) as process:
            try:
                process.communicate()
            except BaseException:
                # Stop the entire native/helper process group before any Docker cleanup.
                # Cancellation leaves durable import intent; no child may restart a server later.
                for sig in (signal.SIGTERM, signal.SIGKILL):
                    try:
                        os.killpg(process.pid, sig)
                    except ProcessLookupError:
                        pass
                process.wait()
                raise
            returncode = process.returncode
    if returncode:
        if name == 'execute':
            for leaf in ('neo4j-preflight.stderr.log', 'neo4j-start.stderr.log', 'neo4j-import.stderr.log'):
                native_log = environment.artifacts / 'logs' / leaf
                if native_log.is_file() and native_log.stat().st_size:
                    print(f'Native diagnostic ({native_log}): {tail(native_log)}', file=sys.stderr)
        raise WorkflowError(f'{name} failed (exit {returncode}): {tail(stderr)}; stdout: {stdout}; stderr: {stderr}')


def bootstrap_runtime(environment, manifest):
    index = environment.artifacts / manifest['index']['path']
    with sqlite3.connect(f'{index.as_uri()}?mode=ro', uri=True) as db:
        nodes = [{'name': row[0], 'properties': []} for row in db.execute('SELECT DISTINCT type FROM nodes ORDER BY type')]
        relations = {}
        for name, from_type, to_type in db.execute('SELECT DISTINCT e.type,f.type,t.type FROM edges e JOIN nodes f ON f.id=e.from_id JOIN nodes t ON t.id=e.to_id ORDER BY e.type'):
            relation = {'name': name, 'from': from_type, 'to': to_type, 'directed': True, 'properties': []}
            if name in relations and relations[name] != relation:
                raise WorkflowError(f'Cannot generate runtime schema: relationship {name} has multiple endpoint type pairs')
            relations[name] = relation
    schema = environment.output / 'runtime-schema.json'
    private_json(schema, {'nodeTypes': nodes, 'relations': list(relations.values())})
    data = environment.output / 'runtime-data.jsonl'
    from tools.graphio.id_codec import canonical_backend_name
    routing = [canonical_backend_name(name) for name in manifest['configuration']['placement']['storageBackends']]
    private_json(data, {'kind': 'metadata', 'idRouting': routing,
                        'idRoutingBits': 16, 'idPayloadBits': 48})
    # JSONL metadata is one record, not a pretty-printed JSON object.
    value = json.loads(data.read_text())
    with data.open('w') as f:
        f.write(json.dumps(value) + '\n')
    environment.runtime_config(manifest['configuration']['placement']['storageBackends'])
    return schema, data


def prepare_runtime(environment, manifest, *, require_bfs=False):
    environment.runtime_config(manifest['configuration']['placement']['storageBackends'])
    try:
        paths = bootstrap_runtime(environment, manifest)
        private_json(environment.output / 'runtime-bootstrap.json', {'schema_available': True})
        return paths
    except WorkflowError as exc:
        if require_bfs:
            raise
        # Native imports support heterogeneous edge endpoints even where the typed BFS schema cannot.
        private_json(environment.output / 'runtime-bootstrap.json', {'schema_available': False, 'reason': str(exc)})
        print('load: runtime schema unavailable (native ingestion remains supported): ' + str(exc), file=sys.stderr)
        return None


def verify_bfs(environment, binary, *, max_vertices=100000):
    binary = Path(binary).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise WorkflowError(f'BFS executable is unavailable: {binary}; build polygraph_bfs or provide --bfs-binary')
    manifest = json.loads((environment.artifacts / 'manifest.json').read_text())
    schema, data = bootstrap_runtime(environment, manifest)
    index = environment.artifacts / manifest['index']['path']
    with sqlite3.connect(f'{index.as_uri()}?mode=ro', uri=True) as db:
        edge = db.execute('SELECT e.from_id FROM edges e JOIN nodes f ON f.id=e.from_id JOIN nodes t ON t.id=e.to_id WHERE f.owner<>t.owner ORDER BY e.id LIMIT 1').fetchone()
        node = edge or db.execute('SELECT id FROM nodes ORDER BY id LIMIT 1').fetchone()
        if node is None:
            result = {'verified': True, 'vertices': 0, 'cross_backend': False}
            private_json(environment.output / 'bfs-verification.json', result)
            return result
        root = node[0]
        expected, frontier = {root: 0}, {root}
        db.execute('CREATE TEMP TABLE frontier(id INTEGER PRIMARY KEY)')
        for depth in (1, 2):
            db.execute('DELETE FROM frontier')
            db.executemany('INSERT INTO frontier VALUES(?)', ((v,) for v in frontier))
            next_frontier = set()
            for a, b in db.execute('SELECT from_id,to_id FROM edges WHERE from_id IN (SELECT id FROM frontier) OR to_id IN (SELECT id FROM frontier)'):
                for v, neighbor in ((a, b), (b, a)):
                    if v in frontier and neighbor not in expected:
                        expected[neighbor] = depth; next_frontier.add(neighbor)
                        if len(expected) > max_vertices:
                            raise WorkflowError(f'BFS reference exceeds {max_vertices} vertices; increase --bfs-max-vertices for this graph')
            frontier = next_frontier
    unsigned = lambda v: v & ((1 << 64) - 1)
    result_path = environment.output / 'bfs.csv'
    stage(environment, 'bfs', [binary, '--logical-graph-schema', schema, '--data', data, '--backend-config', environment.runtime_config_path,
        '--start-uuid', str(unsigned(root)), '--max-depth', '2', '--direction', 'undirected', '--no-logging', '--no-events', '--output-file', result_path])
    with result_path.open(newline='') as f:
        actual = {int(row['vertex_id']): int(row['distance']) for row in csv.DictReader(f, delimiter=';')}
    if actual != {unsigned(v): depth for v, depth in expected.items()}:
        raise WorkflowError(f'Cross-backend BFS differs from the normalized graph; inspect {result_path}')
    owners = {unsigned(v) >> 48 for v in actual}
    result = {'verified': True, 'root': unsigned(root), 'depth': 2, 'vertices': len(actual), 'cross_backend': len(owners) > 1, 'path': str(result_path)}
    if edge and not result['cross_backend']:
        raise WorkflowError('BFS did not reach the expected remote backend')
    private_json(environment.output / 'bfs-verification.json', result)
    return result


def summary(environment, manifest, seconds, running, bfs=None, *, verification='full'):
    cleanup = [sys.executable, str(Path(__file__).resolve()), 'stop', '--state', str(environment.state_path)]
    value = {'state': 'verified' if verification == 'full' else 'imported', 'verification': verification,
        'counts_source': 'live-verification' if verification == 'full' else 'manifest',
        'services_running': running, 'generation': environment.generation, 'output_dir': str(environment.output),
        'containers': {k: v['name'] for k, v in environment.state['containers'].items()}, 'endpoints': {
            'PostgreSQL': f'127.0.0.1:{environment.ports["pg"]}', 'ArangoDB': environment.targets['targets']['ArangoDB']['endpoint'],
            'Neo4J': environment.targets['targets']['Neo4J']['endpoint'], 'Neo4jBolt': f'bolt://127.0.0.1:{environment.ports["bolt"]}'},
        'databases': {name: environment.database for name in environment.targets['targets']}, 'credentials_location': str(environment.credentials_path),
        'runtime_config': str(environment.runtime_config_path), 'runtime_schema_available': (environment.output / 'runtime-schema.json').exists(), 'counts': manifest['counts'],
        'backend_counts': {k: v['counts'] for k, v in manifest['backends'].items()}, 'duration_seconds': seconds,
        'stop_remove_command': shlex.join(cleanup), 'bfs': bfs}
    private_json(environment.output / 'load-result.json', value)
    print(json.dumps(value, indent=2), flush=True)
    return value


def verify_existing(environment, binary, *, bfs_binary=None, max_vertices=100000):
    """Read-only database verification: no provisioning, import, repair or service cleanup."""
    started = time.monotonic()
    result = {'state': 'running', 'generation': environment.generation}
    private_json(environment.output / 'verification-result.json', result)
    try:
        journal = json.loads((environment.artifacts / 'execution-state.json').read_text())
        manifest = json.loads((environment.artifacts / 'manifest.json').read_text())
        targets = json.loads(environment.target_path.read_text())
        target_hash = hashlib.sha256(json.dumps(targets, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        if journal.get('generation_id') != manifest.get('generation_id') or journal.get('target_sha256') != target_hash:
            raise WorkflowError('Execution journal belongs to a different generation/target; refusing verification')
        if (journal.get('state') not in ('imported', 'verified') or
                set(journal.get('backends', {})) != set(manifest.get('backends', {})) or any(
                backend.get('state') not in ('imported', 'verified') for backend in journal.get('backends', {}).values())):
            raise WorkflowError('Native import did not complete; verification cannot resume or repair a partial import')
        for key in ('pg', 'arango', 'neo'):
            inspected = environment.owned_container(key)
            if inspected is None or not inspected['State']['Running']:
                raise WorkflowError(f'Owned {key} service must be running for verification: {environment.names[key]}')
        # verify-live already performs strict artifact reconstruction before accessing databases.
        stage(environment, 'verify-live', [binary, 'verify-live', '--manifest', environment.artifacts / 'manifest.json',
                                         '--target-config', environment.target_path])
        native = json.loads((environment.artifacts / 'live-verification.json').read_text())
        if native.get('state') != 'verified':
            raise WorkflowError('Native verification did not publish a verified report')
        bfs = verify_bfs(environment, bfs_binary, max_vertices=max_vertices) if bfs_binary else None
        result.update(state='verified', native=native, bfs=bfs)
    except BaseException as exc:
        result.update(state='failed', error=str(exc))
    result['duration_seconds'] = time.monotonic() - started
    private_json(environment.output / 'verification-result.json', result)
    environment.state.update(verification=result['state'], verification_duration_seconds=result['duration_seconds'])
    environment.state['status'] = 'running-verified' if result['state'] == 'verified' else 'verification-failed'
    environment.save()
    load_result_path = environment.output / 'load-result.json'
    if load_result_path.is_file():
        loaded = json.loads(load_result_path.read_text())
        loaded.update(state='verified' if result['state'] == 'verified' else 'imported',
                      verification='full' if result['state'] == 'verified' else 'failed',
                      verification_duration_seconds=result['duration_seconds'])
        if result['state'] == 'verified':
            loaded.update(counts_source='live-verification', bfs=result['bfs'])
        else:
            loaded.update(counts_source='manifest', bfs=None)
        private_json(load_result_path, loaded)
    if result['state'] != 'verified':
        raise WorkflowError(f'{result["error"]}\nVerification failed; services and import journals retained unchanged. '
                            f'Report: {environment.output / "verification-result.json"}')
    print(json.dumps(result, indent=2), flush=True)
    return result


def run_cli():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    stop = commands.add_parser('stop', help='Stop/remove only resources owned by this generation; retain artifacts/data/logs')
    stop.add_argument('--state', type=Path, required=True)
    check = commands.add_parser('verify-bfs', help='Validate a running owned environment against its normalized graph')
    check.add_argument('--state', type=Path, required=True)
    check.add_argument('--bfs-binary', type=Path, required=True)
    check.add_argument('--bfs-max-vertices', type=int, default=100000)
    verify = commands.add_parser('verify-load', help='Strict artifacts and live correctness verification of an existing load')
    verify.add_argument('--binary', type=Path, required=True)
    verify.add_argument('--output-dir', type=Path, required=True)
    verify.add_argument('--verify-bfs', action='store_true')
    verify.add_argument('--bfs-binary', type=Path)
    verify.add_argument('--bfs-max-vertices', type=int, default=100000)
    load = commands.add_parser('load')
    load.add_argument('--binary', type=Path, required=True)
    load.add_argument('--adapter', required=True, help='Statically linked input adapter name; the native engine validates it')
    load.add_argument('--input', type=Path, required=True)
    load.add_argument('--placement-config', type=Path, required=True)
    load.add_argument('--docker', action='store_true', required=True)
    load.add_argument('--keep-running', action='store_true')
    load.add_argument('--output-dir', type=Path)
    load.add_argument('--generation-name', default='load-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S'))
    load.add_argument('--threads', type=int, default=3)
    load.add_argument('--memory-limit-mb', type=int, default=256)
    load.add_argument('--chunk-size-mb', type=int, default=64)
    load.add_argument('--max-record-mb', type=int, default=8)
    load.add_argument('--concurrency', type=int, default=3)
    load.add_argument('--arango-concurrency', type=int, default=4)
    load.add_argument('--arango-batch-mb', type=int, default=1)
    load.add_argument('--arango-buffer-mb', type=int, default=128)
    load.add_argument('--partitioner', choices=['none', 'kaminpar'], default='none')
    load.add_argument('--partitions', type=int, default=0)
    load.add_argument('--partition-threads', type=int, default=1)
    load.add_argument('--partition-imbalance', type=float, default=0.03)
    load.add_argument('--partition-preset', choices=['default', 'eco', 'strong'], default='default')
    load.add_argument('--seed', type=int, default=42)
    load.add_argument('--partition-file', type=Path)
    load.add_argument('--partition-map', type=Path)
    load.add_argument('--startup-timeout-seconds', type=int, default=180)
    load.add_argument('--neo-image'); load.add_argument('--arango-image'); load.add_argument('--pg-image')
    load.add_argument('--verify-bfs', action='store_true')
    load.add_argument('--skip-verification', action='store_true')
    load.add_argument('--bfs-binary', type=Path)
    load.add_argument('--bfs-max-vertices', type=int, default=100000)
    args = parser.parse_args()
    if args.command == 'verify-load':
        if args.bfs_max_vertices < 1:
            parser.error('BFS maximum must be positive')
        environment = DockerEnvironment.from_state(args.output_dir / 'environment.json')
        bfs_binary = (args.bfs_binary or args.binary.resolve().with_name('polygraph_bfs')) if args.verify_bfs else None
        verify_existing(environment, args.binary.resolve(), bfs_binary=bfs_binary, max_vertices=args.bfs_max_vertices)
        return
    if args.command in ('stop', 'verify-bfs'):
        environment = DockerEnvironment.from_state(args.state)
        if args.command == 'stop':
            environment.capture_logs(); environment.cleanup()
            print('Removed only owned Docker resources. Artifacts, credentials and bind-mounted data retained at ' + str(environment.output))
        else:
            print(json.dumps(verify_bfs(environment, args.bfs_binary, max_vertices=args.bfs_max_vertices), indent=2))
        return
    if not 1 <= args.concurrency <= 64 or not 1 <= args.startup_timeout_seconds <= 3600 or args.bfs_max_vertices < 1:
        parser.error('concurrency must be 1..64, startup timeout 1..3600, and BFS maximum positive')
    if (not 1 <= args.arango_concurrency <= 32 or not 1 <= args.arango_batch_mb <= 64 or
            not args.arango_batch_mb <= args.arango_buffer_mb <= 1024):
        parser.error('Arango import settings require concurrency 1..32, batch 1..64 MiB, and buffer batch..1024 MiB')
    if args.partitioner == 'kaminpar':
        if (not 2 <= args.partitions < 2**32 - 1 or not 1 <= args.partition_threads <= 256 or
                not math.isfinite(args.partition_imbalance) or not 0 <= args.partition_imbalance <= 1 or
                not 0 <= args.seed <= 2**31 - 1 or args.partition_file):
            parser.error('Partition settings require count >=2, threads 1..256, finite imbalance 0..1, '
                         'signed nonnegative seed and no external partition file')
    elif (args.partitions != 0 or args.partition_threads != 1 or args.partition_imbalance != 0.03 or
          args.partition_preset != 'default'):
        parser.error('Partition controls require --partitioner kaminpar')
    if not 0 <= args.seed <= 2**32 - 1:
        parser.error('Partition seed must be a uint32')
    if args.partition_file and not args.partition_map:
        parser.error('Partition file requires --partition-map')
    if args.partition_map and args.partitioner != 'kaminpar' and not args.partition_file:
        parser.error('Partition map requires --partitioner kaminpar or --partition-file')
    if args.skip_verification and args.verify_bfs:
        parser.error('--skip-verification cannot be combined with --verify-bfs; use verify-load afterward')
    binary = args.binary.resolve()
    bfs_binary = args.bfs_binary or binary.with_name('polygraph_bfs')
    if args.verify_bfs and not bfs_binary.is_file():
        parser.error(f'Build polygraph_bfs or provide --bfs-binary: {bfs_binary}')
    generation = 'polygraph-' + args.generation_name + '-' + secrets.token_hex(5)
    output = args.output_dir or Path.cwd() / 'build/ingestion-load' / generation
    environment = DockerEnvironment(output, generation, images={'neo': args.neo_image, 'arango': args.arango_image, 'pg': args.pg_image},
                                    timeout=args.startup_timeout_seconds, memory_mb=args.memory_limit_mb)
    environment.targets['targets']['ArangoDB'].update(import_concurrency=args.arango_concurrency,
        import_batch_mb=args.arango_batch_mb, import_buffer_mb=args.arango_buffer_mb)
    private_json(environment.target_path, environment.targets)
    started = time.monotonic()
    try:
        prepare_args = [binary, 'prepare', '--adapter', args.adapter, '--input', args.input.resolve(),
            '--placement-config', args.placement_config.resolve(), '--partitioner', args.partitioner, '--output-dir', environment.artifacts,
            '--threads', str(args.threads), '--memory-limit-mb', str(args.memory_limit_mb), '--chunk-size-mb', str(args.chunk_size_mb),
            '--max-record-mb', str(args.max_record_mb), '--seed', str(args.seed)]
        if args.partitioner == 'kaminpar':
            prepare_args += ['--partitions', str(args.partitions), '--partition-threads', str(args.partition_threads),
                '--partition-imbalance', str(args.partition_imbalance), '--partition-preset', args.partition_preset]
        for flag, path in [('--partition-file', args.partition_file), ('--partition-map', args.partition_map)]:
            if path:
                prepare_args += [flag, path.resolve()]
        stage(environment, 'prepare', prepare_args)
        manifest_path = environment.artifacts / 'manifest.json'
        if not args.skip_verification:
            stage(environment, 'verify-strict', [binary, 'verify', '--manifest', manifest_path, '--strict'])
        manifest = json.loads(manifest_path.read_text())
        prepare_runtime(environment, manifest, require_bfs=args.verify_bfs)
        print('load: provisioning isolated Docker environment ' + generation, file=sys.stderr, flush=True)
        environment.provision()
        execute_args = [binary, 'execute', '--manifest', manifest_path, '--target-config', environment.target_path,
                        '--confirm-create', '--concurrency', str(args.concurrency)]
        if args.skip_verification:
            execute_args.append('--skip-verification')
        stage(environment, 'execute', execute_args)
        # Full execute publishes its actual live verification results; do not repeat the scan.
        bfs = verify_bfs(environment, bfs_binary, max_vertices=args.bfs_max_vertices) if args.verify_bfs else None
        environment.capture_logs()
        if args.keep_running:
            for key in ('pg', 'arango', 'neo'):
                inspected = environment.owned_container(key)
                if inspected is None or not inspected['State']['Running']:
                    raise WorkflowError(f'{key} exited after import; inspect the retained server logs')
        else:
            environment.cleanup()
        verification = 'skipped' if args.skip_verification else 'full'
        status = 'imported' if args.skip_verification else 'verified'
        environment.state['status'] = ('running-' + status) if args.keep_running else (status + '-removed')
        environment.state['verification'] = verification; environment.save()
        summary(environment, manifest, time.monotonic() - started, args.keep_running, bfs, verification=verification)
    except BaseException as exc:
        diagnostic = str(exc)
        try:
            environment.capture_logs(); environment.cleanup(remove=False)
        except Exception as cleanup_error:
            diagnostic += '; cleanup diagnostic: ' + str(cleanup_error)
        environment.state.update(status='failed', error=diagnostic, duration_seconds=time.monotonic() - started)
        environment.save()
        raise WorkflowError(f'{diagnostic}\nLogs/state retained: {environment.output}\nNever delete journals to retry partial imports; use a fresh generation.\nStop/remove owned environment: '
                            + shlex.join([sys.executable, str(Path(__file__).resolve()), 'stop', '--state', str(environment.state_path)])) from exc


def main():
    try:
        with cancellation_scope():
            run_cli()
    except WorkflowInterrupted as exc:
        raise WorkflowError(str(exc)) from exc


if __name__ == '__main__':
    try:
        main()
    except (WorkflowError, OSError, ValueError) as error:
        print('Docker load: ' + str(error), file=sys.stderr)
        sys.exit(4)
