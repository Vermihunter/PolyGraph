#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Opt-in three-service test. Creates only uniquely named disposable containers.

python3 tools/ingestion/disposable_live.py --run --binary build/cpu-tests/bin/polygraph_bulk_import \
  --bfs build/cpu-tests/bin/polygraph_bfs --output-dir build/ingestion-live/run-001

Images must already be installed. Never touches existing service containers or volumes.
The owned containers use --rm, no named volumes, and are stopped in finally.
"""
from __future__ import annotations
import argparse
import base64
import csv
import io
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
FIXTURE = ROOT / 'tests/fixtures/ingestion'


def run(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True, text=True, **kwargs)


def request(url, username, password, body=None):
    data = None if body is None else json.dumps(body).encode()
    headers = {'Authorization': 'Basic ' + base64.b64encode(f'{username}:{password}'.encode()).decode(), 'Content-Type': 'application/json'}
    with urllib.request.urlopen(urllib.request.Request(url, data=data, headers=headers), timeout=10) as response:
        return json.load(response)


def wait_for(check, seconds=180):
    deadline = time.monotonic() + seconds
    while True:
        try:
            return check()
        except (Exception,):
            if time.monotonic() >= deadline:
                raise
            time.sleep(1)


def available_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def helper(mode, state_path, extra):
    state = json.loads(Path(state_path).read_text())
    name = state['neo_name']
    if mode == '_neo-preflight':
        result = run(['docker', 'inspect', '--format', '{{.State.Running}}', name], capture_output=True)
        data = Path(state['neo_data'])
        store = data / 'databases' / state['database']
        logs = data / 'transactions' / state['database']
        empty = not store.exists() and not logs.exists()
        print(json.dumps({'database': state['database'], 'offline': result.stdout.strip() == 'false', 'empty': empty}))
    elif mode == '_neo-admin':
        run(['docker', 'run', '--rm', '--pull=never', '--network=none', '--hostname=localhost', '-w', '/data', '--user', f'{os.getuid()}:{os.getgid()}',
             '-e', 'NEO4J_ACCEPT_LICENSE_AGREEMENT=yes', '-e', 'NEO4J_server_directories_data=/data',
             '-e', 'NEO4J_server_directories_logs=/data/logs', '-e', 'HEAP_SIZE=512m',
             '-v', f'{state["neo_data"]}:/data', '-v', f'{state["artifacts"]}:/artifacts:ro',
             '--entrypoint', 'neo4j-admin', state['neo_image'], *extra, '--additional-config=/artifacts/import.conf'])
    elif mode == '_neo-start':
        run(['docker', 'start', name])
    else:
        raise ValueError(mode)


def main():
    if len(sys.argv) >= 3 and sys.argv[1].startswith('_neo-'):
        helper(sys.argv[1], sys.argv[2], sys.argv[3:])
        return
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--run', action='store_true', help='Explicitly opt in to disposable live tests')
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--bfs', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True, help='Must not already exist; use an ignored build directory')
    parser.add_argument('--unique-output', action='store_true', help='Append a unique child directory (for repeated CTest runs)')
    parser.add_argument('--workflow', choices=['native', 'python'], default='native', help='Compare native execution with the existing Python loaders on fresh targets')
    parser.add_argument('--adapter', choices=['ldbc-snb', 'polygraph-jsonl'], default='ldbc-snb')
    parser.add_argument('--neo-image', default='polygraph-neo4j:5.26-enterprise')
    parser.add_argument('--arango-image', default='polygraph-arangodb:3.12')
    parser.add_argument('--pg-image', default='postgres:17')
    args = parser.parse_args()
    if not args.run:
        parser.error('--run is required; no services are touched by default')
    output = args.output_dir.resolve()
    if args.unique_output:
        output = output / ('run-' + secrets.token_hex(6))
    output.mkdir(parents=True, exist_ok=False)
    binary, bfs = args.binary.resolve(), args.bfs.resolve()
    token = secrets.token_hex(5)
    prefix = 'polygraph-bulk-it-' + token
    database = 'bulk-it-' + token
    names = {backend: prefix + '-' + backend for backend in ('neo', 'arango', 'pg')}
    ports = {backend: available_port() for backend in names}
    password = secrets.token_urlsafe(24)
    env = os.environ.copy()
    env.update(POSTGRES_PASSWORD=password, ARANGO_ROOT_PASSWORD=password, NEO4J_AUTH='neo4j/' + password, POLYGRAPH_BULK_TEST_PASSWORD=password)
    os.environ['POLYGRAPH_BULK_TEST_PASSWORD'] = password
    (output / 'neo-data').mkdir(mode=0o777)
    os.chmod(output / 'neo-data', 0o777)
    artifacts = output / 'artifacts'
    input_path = FIXTURE / 'raw'
    if args.adapter == 'polygraph-jsonl':
        records = [json.loads(line) for line in (FIXTURE / 'canonical.jsonl').read_text().splitlines()]
        records[1]['properties'].update(empty_list=[], odd_strings=['', 'x\x1fy'], enabled=True, score=1.25, sparse='value')
        # Same label: a missing property, explicit null, and explicit empty string.
        extra = [{'kind': 'node', 'uuid': (1 << 48) + 100 + i, 'type': 'Person', 'source': 'Neo4J', 'properties': properties} for i, properties in enumerate([{}, {'sparse': None}, {'sparse': ''}])]
        first_edge = next(i for i, r in enumerate(records) if r.get('kind') == 'relationship')
        records[first_edge:first_edge] = extra
        input_path = output / 'input.jsonl'
        input_path.write_text(''.join(json.dumps(r, ensure_ascii=False) + '\n' for r in records))
    run([binary, 'prepare', '--adapter', args.adapter, '--input', input_path, '--placement-config', FIXTURE / 'placement.json', '--output-dir', artifacts])
    state = {'neo_name': names['neo'], 'neo_data': str(output / 'neo-data'), 'artifacts': str(artifacts), 'database': database, 'neo_image': args.neo_image}
    (artifacts / 'import.conf').write_text('server.directories.data=/data\nserver.directories.logs=/data/logs\nserver.directories.transaction.logs.root=/data/transactions\n')
    state_path = output / 'runner-state.json'
    state_path.write_text(json.dumps(state, indent=2))
    helper_argv = [sys.executable, str(Path(__file__).resolve())]
    targets = {'targets': {
        'PostgreSQL': {'database': database, 'host': '127.0.0.1', 'port': ports['pg'], 'username': 'postgres', 'password_env': 'POLYGRAPH_BULK_TEST_PASSWORD'},
        'ArangoDB': {'database': database, 'endpoint': f'http://127.0.0.1:{ports["arango"]}', 'username': 'root', 'password_env': 'POLYGRAPH_BULK_TEST_PASSWORD'},
        'Neo4J': {'database': database, 'offline': True, 'endpoint': f'http://127.0.0.1:{ports["neo"]}', 'username': 'neo4j', 'password_env': 'POLYGRAPH_BULK_TEST_PASSWORD',
                  'artifact_root': '/artifacts', 'runner': [*helper_argv, '_neo-admin', str(state_path)],
                  'preflight_command': [*helper_argv, '_neo-preflight', str(state_path)],
                  'start_command': [*helper_argv, '_neo-start', str(state_path)], 'startup_attempts': 45}}}
    target_path = output / 'targets.json'
    target_path.write_text(json.dumps(targets, indent=2))
    owned = []
    try:
        # --rm ensures cleanup only of this test's newly created containers.
        run(['docker', 'run', '-d', '--rm', '--pull=never', '--name', names['pg'], '-p', f'127.0.0.1:{ports["pg"]}:5432', '-e', 'POSTGRES_PASSWORD', '-e', f'POSTGRES_DB={database}', args.pg_image], env=env)
        owned.append(names['pg'])
        run(['docker', 'run', '-d', '--rm', '--pull=never', '--name', names['arango'], '-p', f'127.0.0.1:{ports["arango"]}:8529', '-e', 'ARANGO_ROOT_PASSWORD', args.arango_image,
             '--rocksdb.block-cache-size=67108864', '--cache.size=33554432'], env=env)
        owned.append(names['arango'])
        run(['docker', 'create', '--pull=never', '--name', names['neo'], '-p', f'127.0.0.1:{ports["neo"]}:7474', '-e', 'NEO4J_AUTH',
             '-e', 'NEO4J_ACCEPT_LICENSE_AGREEMENT=yes', '-e', f'NEO4J_initial_dbms_default__database={database}',
             '-e', 'NEO4J_server_memory_heap_initial__size=256m', '-e', 'NEO4J_server_memory_heap_max__size=512m', '-e', 'NEO4J_server_memory_pagecache_size=128m',
             '-v', f'{output / "neo-data"}:/data', args.neo_image], env=env)
        owned.append(names['neo'])
        wait_for(lambda: run(['docker', 'exec', names['pg'], 'pg_isready', '-U', 'postgres', '-d', database], capture_output=True))
        arango = targets['targets']['ArangoDB']['endpoint']
        wait_for(lambda: request(arango + '/_api/version', 'root', password))
        request(arango + '/_api/database', 'root', password, {'name': database})
        # Runtime integration: use the existing backend adapters and distributed BFS.
        from tools.graphio.domain import _backend_entry
        placement = json.loads((FIXTURE / 'placement.json').read_text())
        entries = [_backend_entry(n, True, database) for n in placement['storageBackends']]
        for entry in entries:
            backend = entry['id']
            entry['connection']['credentials'] = {'username': 'postgres' if backend == 'PostgreSQL' else 'root' if backend == 'ArangoDB' else 'neo4j', 'password': password}
            entry['connection']['database'] = database
            entry['connection']['endpoint'] = (f'127.0.0.1:{ports["pg"]}' if backend == 'PostgreSQL' else
                f'{arango}/_db/{database}/_api/cursor' if backend == 'ArangoDB' else f'http://127.0.0.1:{ports["neo"]}/db/{database}/tx/commit')
        backend_path = output / 'runtime.secret.json'
        with os.fdopen(os.open(backend_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as config_file:
            json.dump({'idRouting': placement['storageBackends'], 'backends': entries}, config_file)
        # Invalid preflight must not import anything, even when an unrelated collection is empty.
        bad_database = database + '-preflight'
        request(arango + '/_api/database', 'root', password, {'name': bad_database})
        request(arango + f'/_db/{bad_database}/_api/collection', 'root', password, {'name': 'unrelated_empty'})
        bad_targets = json.loads(json.dumps(targets))
        bad_targets['targets']['ArangoDB']['database'] = bad_database
        bad_path = output / 'preflight-targets.json'
        bad_path.write_text(json.dumps(bad_targets))
        rejected = subprocess.run([str(binary), 'execute', '--manifest', str(artifacts / 'manifest.json'), '--target-config', str(bad_path), '--confirm-create'], env=env, capture_output=True)
        if rejected.returncode == 0 or (artifacts / 'execution-state.json').exists():
            raise AssertionError('Unrelated empty Arango collection was not rejected before import')
        canonical_input = input_path if args.adapter == 'polygraph-jsonl' else FIXTURE / 'canonical.jsonl'
        imported_at = time.perf_counter()
        if args.workflow == 'native':
            run([binary, 'execute', '--manifest', artifacts / 'manifest.json', '--target-config', target_path, '--confirm-create'], env=env)
        else:
            # The old loader's reset=False path skips Neo4j. Reuse its fresh insertion path,
            # replacing only destructive reset with a read-only assertion on this new store.
            run(['docker', 'start', names['neo']])
            neo_url = targets['targets']['Neo4J']['endpoint'] + f'/db/{database}/tx/commit'
            wait_for(lambda: request(neo_url, 'neo4j', password, {'statements': [{'statement': 'RETURN 1'}]}))
            from tools.graphio.loaders import (Neo4jHttpAdapter, ArangoHttpAdapter, PostgreSqlAdapter,
                read_backend_connections, read_id_routing_layout, build_endpoint_directory, _load_neo4j_fresh,
                load_source, load_postgresql_source)
            class EmptyNeo4j(Neo4jHttpAdapter):
                def reset(self, *, batch_size=1000):
                    count = self._cypher('MATCH (n) RETURN count(n)')['results'][0]['data'][0]['row'][0]
                    if count != 0:
                        raise RuntimeError('Refusing to reset a nonempty comparison target')
            connections = read_backend_connections(backend_path)
            layout = read_id_routing_layout(backend_path)
            with build_endpoint_directory(canonical_input, layout, index_path=output / 'legacy-index.sqlite3') as directory:
                _load_neo4j_fresh(input_graph=canonical_input, adapter=EmptyNeo4j(connections['Neo4J']), routing_layout=layout, batch_size=20000, endpoint_directory=directory)
                load_source(input_graph=canonical_input, source='ArangoDB', adapter=ArangoHttpAdapter(connections['ArangoDB'], import_workers=4), endpoint_directory=directory, routing_layout=layout, reset=False, batch_size=20000)
                load_postgresql_source(input_graph=canonical_input, adapter=PostgreSqlAdapter(connections['PostgreSQL']), endpoint_directory=directory, routing_layout=layout, reset=False, batch_size=20000)
        import_elapsed = time.perf_counter() - imported_at
        metrics = {'workflow': args.workflow, 'adapter': args.adapter, 'import_and_startup_seconds': import_elapsed,
                   'images': {'neo4j': args.neo_image, 'arango': args.arango_image, 'postgres': args.pg_image},
                   'cleanup': 'fresh per-run containers, no named volumes',
                   'settings': 'Neo4j 512MiB heap/128MiB page cache; Arango 64MiB block/32MiB cache; PG image defaults; no optional traversal indexes',
                   'caveat': 'Tiny correctness fixture, single run; includes startup and different native post-validation work, no speedup claim'}
        if args.workflow == 'native':
            metrics['native_stages'] = json.loads((artifacts / 'execution-state.json').read_text())
        (output / 'import-measurement.json').write_text(json.dumps(metrics, indent=2))
        run([binary, 'verify-live', '--manifest', artifacts / 'manifest.json', '--target-config', target_path], env=env)
        from tools.ldbc_snb.spec import EDGE_SPECS
        relation_types = {s.type_name: (s.from_type, s.to_type) for s in EDGE_SPECS.values()}
        schema = {'nodeTypes': [{'name': n, 'source': s, 'properties': []} for n, s in placement['nodeSources'].items()],
                  'relations': [{'name': n, 'source': s, 'from': f, 'to': t, 'directed': True, 'properties': []} for n, s in placement['relationshipSources'].items() for f, t in [relation_types[n]]]}
        schema_path = output / 'schema.json'
        schema_path.write_text(json.dumps(schema))
        golden = json.loads((FIXTURE / 'python-golden.json').read_text())
        if args.adapter == 'polygraph-jsonl':
            golden['nodes'] = [{k: v for k, v in r.items() if k != 'kind'} for r in records if r.get('kind') == 'node']
        # Compare properties as stored by the actual native tools, including Neo4j array repairs.
        for node in golden['nodes']:
            if node['source'] == 'Neo4J':
                url = targets['targets']['Neo4J']['endpoint'] + f'/db/{database}/tx/commit'
                response = request(url, 'neo4j', password, {'statements': [{'statement': 'MATCH (n:Person {localId:$id}) RETURN properties(n)', 'parameters': {'id': node['uuid'] & ((1 << 48) - 1)}}]})
                if response['errors']:
                    raise AssertionError(response['errors'])
                stored = response['results'][0]['data'][0]['row'][0]
                expected = {**{k: v for k, v in node['properties'].items() if v is not None}, 'localId': node['uuid'] & ((1 << 48) - 1), 'type': node['type'], 'source': node['source'], 'is_proxy': False}
            elif node['source'] == 'ArangoDB':
                stored = request(arango + f'/_db/{database}/_api/document/{node["type"]}/{node["uuid"]}', 'root', password)
                expected = {**node['properties'], '_key': str(node['uuid']), 'uuid': node['uuid'], 'type': node['type'], 'source': node['source']}
                stored.pop('_id', None); stored.pop('_rev', None)
            else:
                signed = node['uuid'] - (1 << 64)
                result = run(['docker', 'exec', names['pg'], 'psql', '-U', 'postgres', '-d', database, '-Atc', f'SELECT properties FROM polygraph_nodes WHERE id={signed}'], capture_output=True)
                stored = json.loads(result.stdout)
                expected = node['properties']
            if stored != expected:
                raise AssertionError(f'Native properties differ: {stored} != {expected}')
        expected_ids = [n['uuid'] for n in golden['nodes']]
        for root in expected_ids:
            result_path = output / f'bfs-{root}.csv'
            run([bfs, '--logical-graph-schema', schema_path, '--data', canonical_input, '--backend-config', backend_path,
                 '--no-logging', '--no-events', '--start-uuid', str(root), '--direction', 'undirected', '--output-file', result_path], env=env, stdout=subprocess.DEVNULL)
            actual = {int(r['vertex_id']): int(r['distance']) for r in csv.DictReader(result_path.open(), delimiter=';')}
            adjacency = {i: set() for i in expected_ids}
            for edge in golden['edges']:
                adjacency[edge['from']].add(edge['to']); adjacency[edge['to']].add(edge['from'])
            expected, frontier = {root: 0}, [root]
            while frontier:
                vertex = frontier.pop(0)
                for neighbor in adjacency[vertex]:
                    if neighbor not in expected:
                        expected[neighbor] = expected[vertex] + 1; frontier.append(neighbor)
            if actual != expected:
                raise AssertionError(f'BFS differs: {actual} != {expected}')
        (output / 'integration-result.json').write_text(json.dumps({'state': 'passed', 'exact_counts_ids_endpoints': True, 'runtime_bfs_roots': len(expected_ids)}, indent=2))
        print(f'Disposable three-backend {args.workflow} import and PolyGraph BFS tests passed')
    finally:
        for name in reversed(owned):
            with (output / f'{name}.log').open('w') as log:
                subprocess.run(['docker', 'logs', name], stdout=log, stderr=subprocess.STDOUT)
            # Only stop exact names generated and created by this invocation. No volume reset/drop.
            subprocess.run(['docker', 'stop', '-t', '10', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(['docker', 'rm', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        secret_path = output / 'runtime.secret.json'
        secret_path.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
