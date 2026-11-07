#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Opt-in end-to-end tests of the public C++ load CLI, including failure and cleanup."""
from __future__ import annotations
import argparse
import base64
import json
from pathlib import Path
import secrets
import sqlite3
import shlex
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.ingestion.docker_environment import DockerEnvironment, private_json


def run(argv, **kwargs):
    return subprocess.run(list(map(str, argv)), check=True, text=True, **kwargs)


def check_partition_corruption(environment, binary):
    """Damage/restore only this dedicated fixture; exact live checks must notice."""
    from tools.ingestion.docker_environment import request
    from tools.graphio.domain import sanitize_name
    from tools.graphio.id_codec import local_payload
    manifest = json.loads((environment.artifacts / 'manifest.json').read_text())
    journal = environment.artifacts / 'execution-state.json'
    before = journal.read_bytes()
    checks = []
    with sqlite3.connect(f'{(environment.artifacts / manifest["index"]["path"]).as_uri()}?mode=ro', uri=True) as db:
        for backend in ('PostgreSQL', 'Neo4J', 'ArangoDB'):
            for edge in (False, True):
                if edge:
                    row = db.execute('SELECT e.id,e.type,e.owner,e.properties FROM edges e JOIN replicas r ON r.edge_id=e.id '
                                     'WHERE r.backend=? ORDER BY e.id LIMIT 1', (backend,)).fetchone()
                else:
                    row = db.execute('SELECT id,type,owner,properties FROM nodes WHERE owner=? ORDER BY id LIMIT 1', (backend,)).fetchone()
                assert row is not None
                ident, kind, owner, properties = row
                field = 'to_partition_id' if edge else 'partition_id'
                original = json.loads(properties)[field]
                def change(value):
                    if backend == 'PostgreSQL':
                        table = 'polygraph_edges' if edge else 'polygraph_nodes'
                        sql = f"UPDATE public.{table} SET properties=jsonb_set(properties,'{{{field}}}',to_jsonb({int(value)}::bigint)) WHERE id={int(ident)}"
                        target = environment.targets['targets'][backend]
                        environment.docker(['exec', environment.state['containers']['pg']['id'], 'psql', '-U', target['username'],
                            '-d', environment.database, '-v', 'ON_ERROR_STOP=1', '-c', sql])
                    elif backend == 'Neo4J':
                        native = owner == backend
                        key = 'localId' if native else 'uuid'
                        lookup = local_payload(ident) if native else ident
                        match = f'MATCH ()-[n:{sanitize_name(kind)} {{{key}:$id}}]->()' if edge else 'MATCH (n:POLYGRAPHVertex {localId:$id})'
                        result = request(environment.targets['targets'][backend]['endpoint'] + '/db/' + environment.database + '/tx/commit',
                            'neo4j', environment.password, {'statements': [{'statement': match + f' SET n.{field}=$value RETURN count(n)',
                                'parameters': {'id': lookup, 'value': value}}]})
                        assert result['results'][0]['data'][0]['row'][0] == 1
                    else:
                        url = environment.targets['targets'][backend]['endpoint'] + '/_db/' + environment.database + '/_api/document/' + sanitize_name(kind) + '/' + str(ident & ((1 << 64) - 1))
                        auth = base64.b64encode(('root:' + environment.password).encode()).decode()
                        req = urllib.request.Request(url, method='PATCH', data=json.dumps({field: value}).encode(),
                            headers={'Authorization': 'Basic ' + auth, 'Content-Type': 'application/json'})
                        with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(req, timeout=10) as response:
                            assert not json.load(response).get('error')
                try:
                    change(original + 10000)
                    failed = subprocess.run(list(map(str, [binary, 'verify-load', '--output-dir', environment.output])),
                                            capture_output=True, text=True)
                    assert failed.returncode != 0 and 'metadata mismatch' in failed.stderr, failed.stderr
                    assert journal.read_bytes() == before
                    checks.append({'backend': backend, 'kind': 'edge' if edge else 'node', 'corruption_detected': True})
                finally:
                    change(original)
    run([binary, 'verify-load', '--output-dir', environment.output], capture_output=True)
    assert journal.read_bytes() == before
    return checks


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', action='store_true', required=True)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--bfs', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--partitioner', choices=['none', 'kaminpar'], default='none')
    p.add_argument('--partitions', type=int, default=2)
    p.add_argument('--cases', nargs='+', choices=['keep', 'remove', 'fail', 'deferred', 'parallel', 'canonical', 'mapped'])
    args = p.parse_args()
    root = args.output_dir.resolve() / ('run-' + secrets.token_hex(6))
    root.mkdir(parents=True)
    before = set(run(['docker', 'ps', '-aq', '--no-trunc'], capture_output=True).stdout.split())
    state_format = '{{if .Config.Labels}}{{index .Config.Labels "org.polygraph.ingestion.generation"}}{{end}}|{{.State.Running}}|{{.State.StartedAt}}'
    untouched_states = {}
    for cid in before:
        value = run(['docker', 'inspect', '--format', state_format, cid], capture_output=True).stdout.strip()
        if value.split('|', 1)[0] in ('', '<no value>'):
            untouched_states[cid] = value
    common = [args.binary.resolve(), 'load', '--adapter', 'ldbc-snb', '--input', ROOT / 'tests/fixtures/ingestion/raw',
              '--placement-config', ROOT / 'tests/fixtures/ingestion/placement.json', '--docker', '--generation-name', 'e2e']
    if args.partitioner == 'kaminpar':
        common += ['--partitioner', 'kaminpar', '--partitions', str(args.partitions)]
    results = []
    modes = args.cases or ['keep', 'remove', 'fail', 'deferred', 'parallel'] + (['canonical', 'mapped'] if args.partitioner == 'kaminpar' else [])
    if args.partitioner != 'kaminpar' and any(mode in ('canonical', 'mapped') for mode in modes):
        p.error('canonical/mapped cases require --partitioner kaminpar')
    for mode in modes:
        output = root / mode
        command = [*common, '--output-dir', output]
        if mode in ('keep', 'canonical', 'mapped'):
            command += ['--keep-running', '--verify-bfs', '--bfs-binary', args.bfs.resolve()]
        if mode == 'canonical':
            command[command.index('--adapter') + 1] = 'polygraph-jsonl'
            command[command.index('--input') + 1] = ROOT / 'tests/fixtures/ingestion/canonical.jsonl'
        if mode == 'mapped':
            command[command.index('--partitions') + 1] = '3'
            mapping = root / 'mapping.json'
            mapping.write_text(json.dumps({'0': 'Neo4J', '1': 'ArangoDB', '2': 'PostgreSQL'}))
            command += ['--partition-map', mapping]
        if mode == 'fail':
            # A cached non-Neo4j image deliberately makes only the native runner fail.
            command += ['--neo-image', 'postgres:17', '--skip-verification']
        if mode == 'deferred':
            command += ['--keep-running', '--skip-verification']
        if mode == 'parallel':
            from tools.ingestion.benchmark import dataset
            source = root / 'parallel-input'
            dataset(source, 6000)
            command[command.index('--input') + 1] = source
            command += ['--keep-running', '--skip-verification', '--arango-concurrency', '4',
                        '--arango-batch-mb', '1', '--arango-buffer-mb', '8']
        with (root / f'{mode}.stdout.log').open('w') as out, (root / f'{mode}.stderr.log').open('w') as err:
            completed = subprocess.run(list(map(str, command)), stdout=out, stderr=err)
        environment = DockerEnvironment.from_state(output / 'environment.json')
        try:
            if mode == 'fail':
                assert completed.returncode != 0
                assert environment.state['status'] == 'failed'
                journal = json.loads((environment.artifacts / 'execution-state.json').read_text())
                assert journal['state'] == 'failed'
                assert journal['backends']['Neo4J']['state'] == 'failed'
                assert (environment.output / 'logs/execute.stderr.log').stat().st_size
                retry = subprocess.run(list(map(str, command)), capture_output=True, text=True)
                assert retry.returncode != 0 and 'fresh output' in retry.stderr
                assert journal == json.loads((environment.artifacts / 'execution-state.json').read_text())
            else:
                assert completed.returncode == 0, (root / f'{mode}.stderr.log').read_text()
                report = json.loads((output / 'load-result.json').read_text())
                expected_counts = {'nodes': 18000, 'edges': 18000} if mode == 'parallel' else {'nodes': 3, 'edges': 3}
                assert report['counts'] == expected_counts
                assert (environment.credentials_path.stat().st_mode & 0o777) == 0o600
                if mode == 'parallel':
                    journal_path = environment.artifacts / 'execution-state.json'
                    original_journal = journal_path.read_bytes()
                    imported = json.loads(original_journal)['backends']['ArangoDB']['import']
                    assert imported['created'] == 36000
                    batching = imported['batching']
                    assert batching['workers'] == 4 and batching['peak_active_batches'] >= 2
                    assert batching['peak_buffered_bytes'] <= 8 * 1024 * 1024
                    assert batching['peak_queued_batches'] <= 8
                    assert batching['completed_batches'] > 5
                    run([args.binary.resolve(), 'verify-load', '--output-dir', output,
                         '--verify-bfs', '--bfs-binary', args.bfs.resolve()], capture_output=True)
                    assert journal_path.read_bytes() == original_journal
                    verified = json.loads((output / 'verification-result.json').read_text())
                    assert verified['state'] == 'verified' and verified['bfs']['cross_backend']
                    manifest = json.loads((environment.artifacts / 'manifest.json').read_text())
                    index = environment.artifacts / manifest['index']['path']
                    from tools.ingestion.docker_environment import request
                    with sqlite3.connect(f'{index.as_uri()}?mode=ro', uri=True) as db:
                        for order in ('ASC', 'DESC'):
                            ident, properties = db.execute('SELECT id,properties FROM nodes WHERE type=? ORDER BY id ' + order + ' LIMIT 1', ('Post',)).fetchone()
                            key = str(ident & ((1 << 64) - 1))
                            stored = request(environment.targets['targets']['ArangoDB']['endpoint'] + '/_db/' +
                                environment.database + '/_api/document/Post/' + key, 'root', environment.password)
                            for name, value in json.loads(properties).items():
                                assert stored[name] == value, (name, stored[name], value)
                elif mode == 'deferred':
                    assert report['state'] == 'imported' and report['verification'] == 'skipped'
                    assert report['counts_source'] == 'manifest'
                    assert not (environment.artifacts / 'live-verification.json').exists()
                    assert not (environment.output / 'logs/verify-strict.stdout.log').exists()
                    assert not (environment.output / 'logs/verify-live.stdout.log').exists()
                    journal_path = environment.artifacts / 'execution-state.json'
                    original_journal = journal_path.read_bytes()
                    journal = json.loads(original_journal)
                    assert journal['state'] == 'imported'
                    assert all(b['state'] == 'imported' and 'verification' not in b for b in journal['backends'].values())
                    verify_command = [args.binary.resolve(), 'verify-load', '--output-dir', output,
                                      '--verify-bfs', '--bfs-binary', args.bfs.resolve()]
                    run(verify_command, capture_output=True)
                    checked = json.loads((output / 'load-result.json').read_text())
                    assert checked['state'] == 'verified' and checked['bfs']['cross_backend']
                    assert checked['duration_seconds'] == report['duration_seconds']
                    assert checked['verification_duration_seconds'] > 0
                    assert journal_path.read_bytes() == original_journal
                    # Damage only this fresh fixture, keeping counts unchanged. Exact verification must catch it.
                    rows = [json.loads(s) for s in (ROOT / 'tests/fixtures/ingestion/canonical.jsonl').read_text().splitlines()]
                    post = next(r for r in rows if r.get('type') == 'Post' and r.get('kind') != 'relationship')
                    url = environment.targets['targets']['ArangoDB']['endpoint'] + '/_db/' + environment.database + '/_api/document/Post/' + str(post['uuid'])
                    auth = base64.b64encode(('root:' + environment.password).encode()).decode()
                    def patch_type(value):
                        req = urllib.request.Request(url, method='PATCH', data=json.dumps({'type': value}).encode(),
                              headers={'Authorization': 'Basic ' + auth, 'Content-Type': 'application/json'})
                        with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(req, timeout=10) as response:
                            assert not json.load(response).get('error')
                    try:
                        patch_type('INTENTIONALLY_CORRUPT')
                        failed = subprocess.run(list(map(str, verify_command)), capture_output=True, text=True)
                        assert failed.returncode != 0 and 'mismatch' in failed.stderr
                        assert json.loads((output / 'verification-result.json').read_text())['state'] == 'failed'
                        corrupt_report = json.loads((output / 'load-result.json').read_text())
                        assert corrupt_report['verification'] == 'failed'
                        assert corrupt_report['bfs'] is None and corrupt_report['counts_source'] == 'manifest'
                        assert journal_path.read_bytes() == original_journal
                        for key in ('pg', 'arango', 'neo'):
                            assert environment.owned_container(key)['State']['Running']
                    finally:
                        patch_type('Post')
                    run(verify_command, capture_output=True)
                    assert json.loads((output / 'verification-result.json').read_text())['state'] == 'verified'
                    assert journal_path.read_bytes() == original_journal
                else:
                    assert json.loads((environment.artifacts / 'live-verification.json').read_text())['state'] == 'verified'
                partition_checks = []
                if args.partitioner == 'kaminpar':
                    checked = json.loads((environment.artifacts / 'live-verification.json').read_text())
                    assert all(result['partition_metadata'] for result in checked['backends'].values())
                    if mode == 'keep':
                        partition_checks = check_partition_corruption(environment, args.binary.resolve())
                if mode in ('keep', 'canonical', 'mapped'):
                    assert report['services_running']
                    assert report['bfs']['verified'] and report['bfs']['cross_backend']
                    for key in ('pg', 'arango', 'neo'):
                        assert environment.owned_container(key)['State']['Running']
                    run(shlex.split(report['stop_remove_command']))
                    # The printed cleanup command is safe to repeat.
                    run(shlex.split(report['stop_remove_command']))
                elif mode == 'remove':
                    assert not report['services_running']
                    assert all(environment.owned_container(key) is None for key in environment.state['containers'])
            results.append({'mode': mode, 'passed': True, 'output': str(output),
                            'partition_metadata_corruption_checks': partition_checks if mode != 'fail' else []})
        finally:
            environment.capture_logs()
            environment.cleanup()
    after = set(run(['docker', 'ps', '-aq', '--no-trunc'], capture_output=True).stdout.split())
    assert before <= after, 'A pre-existing container disappeared'
    for cid, expected in untouched_states.items():
        actual = run(['docker', 'inspect', '--format', state_format, cid], capture_output=True).stdout.strip()
        assert actual == expected, 'A pre-existing non-ingestion container was stopped/restarted'
    private_json(root / 'result.json', {'passed': True, 'cases': results, 'preexisting_containers_preserved': len(before), 'preexisting_states_preserved': len(untouched_states)})
    print(json.dumps({'passed': True, 'report': str(root / 'result.json')}, indent=2))


if __name__ == '__main__':
    main()
