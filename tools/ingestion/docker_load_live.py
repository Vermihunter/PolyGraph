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
import shlex
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.ingestion.docker_environment import DockerEnvironment, private_json


def run(argv, **kwargs):
    return subprocess.run(list(map(str, argv)), check=True, text=True, **kwargs)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', action='store_true', required=True)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--bfs', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
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
    results = []
    for mode in ('keep', 'remove', 'fail', 'deferred'):
        output = root / mode
        command = [*common, '--output-dir', output]
        if mode == 'keep':
            command += ['--keep-running', '--verify-bfs', '--bfs-binary', args.bfs.resolve()]
        if mode == 'fail':
            # A cached non-Neo4j image deliberately makes only the native runner fail.
            command += ['--neo-image', 'postgres:17', '--skip-verification']
        if mode == 'deferred':
            command += ['--keep-running', '--skip-verification']
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
                assert report['counts'] == {'nodes': 3, 'edges': 3}
                assert (environment.credentials_path.stat().st_mode & 0o777) == 0o600
                if mode == 'deferred':
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
                if mode == 'keep':
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
            results.append({'mode': mode, 'passed': True, 'output': str(output)})
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
