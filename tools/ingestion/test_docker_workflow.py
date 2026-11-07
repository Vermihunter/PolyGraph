"""Service-free lifecycle/ownership tests; live tests separately exercise the native tools."""
from __future__ import annotations
import json
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import time
import signal
import tempfile
import unittest
from unittest.mock import patch

from tools.ingestion.docker_environment import DockerEnvironment, WorkflowError, WorkflowInterrupted, OWNER_LABEL


class FakeDocker:
    def __init__(self):
        self.containers = {}
        self.calls = []
        self.networks = {}
        self.fail_start = False

    def __call__(self, argv, **kwargs):
        self.calls.append((list(argv), kwargs.get('env', {})))
        args = list(argv)[1:]
        out = ''
        if args[:2] == ['network', 'create']:
            key, value = args[args.index('--label') + 1].split('=', 1)
            out = 'b' * 64
            self.networks[out] = {'Id': out, 'Name': args[-1], 'Labels': {key: value}}
        elif args[:2] == ['network', 'inspect']:
            value = self.networks.get(args[-1]) or next((v for v in self.networks.values() if v.get('Name') == args[-1]), None)
            if value is None:
                return subprocess.CompletedProcess(argv, 1, '', 'error: network not found')
            out = json.dumps([value])
        elif args[:2] == ['network', 'rm']:
            del self.networks[args[-1]]
        elif args[0] == 'info':
            out = 'test-daemon'
        elif args[:2] == ['image', 'inspect']:
            out = json.dumps([{'Id': 'sha256:' + 'a' * 64}])
        elif args[0] == 'create':
            name = args[args.index('--name') + 1]
            label = args[args.index('--label') + 1]
            key, value = label.split('=', 1)
            cid = f'{len(self.containers) + 1:064x}'
            self.containers[cid] = {'Id': cid, 'Name': '/' + name, 'Config': {'Labels': {key: value}}, 'State': {'Running': False}, 'Mounts': []}
            out = cid
        elif args[0] == 'inspect':
            value = self.containers.get(args[-1]) or next((v for v in self.containers.values() if v.get('Name') == '/' + args[-1]), None)
            if value is None:
                return subprocess.CompletedProcess(argv, 1, '', 'error: no such object')
            out = json.dumps([value])
        elif args[0] == 'start':
            if self.fail_start:
                return subprocess.CompletedProcess(argv, 1, '', 'start rejected')
            self.containers[args[-1]]['State']['Running'] = True
        elif args[0] == 'stop':
            self.containers[args[-1]]['State']['Running'] = False
        elif args[0] == 'rm':
            del self.containers[args[-1]]
        elif args[0] in {'exec', 'logs'}:
            pass
        else:
            raise AssertionError(args)
        return subprocess.CompletedProcess(argv, 0, out, '')


class DockerEnvironmentTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        ports = patch('tools.ingestion.docker_environment.available_port', side_effect=iter(range(41000, 41100)))
        ports.start(); self.addCleanup(ports.stop)
        self.fake = FakeDocker()
        self.env = DockerEnvironment(Path(self.tmp.name), 'polygraph-test-abc123', runner=self.fake)

    @patch('tools.ingestion.docker_environment.request', return_value={'error': False})
    def test_fresh_provisioning_private_credentials_and_owned_cleanup(self, http):
        self.env.provision()
        self.assertEqual(len(self.fake.containers), 3)
        self.assertEqual(self.env.credentials_path.stat().st_mode & 0o777, 0o600)
        self.assertFalse(self.fake.containers[self.env.state['containers']['neo']['id']]['State']['Running'])
        self.assertEqual(self.env.targets['targets']['Neo4J']['database'], self.env.database)
        self.assertTrue(self.env.targets['targets']['Neo4J']['start_command'])
        self.assertIn('127.0.0.1', self.env.env['NO_PROXY'])
        self.assertIn('localhost', self.env.env['no_proxy'])
        for argv, _ in self.fake.calls:
            self.assertNotIn(self.env.password, ' '.join(argv))
        unrelated = {'Id': 'f' * 64, 'Config': {'Labels': {OWNER_LABEL: 'another-generation'}}, 'State': {'Running': True}}
        self.fake.containers[unrelated['Id']] = unrelated
        self.env.cleanup()
        self.assertEqual(self.fake.containers, {unrelated['Id']: unrelated})
        self.assertTrue((self.env.output / 'logs').is_dir())

    @patch('tools.ingestion.docker_environment.request', return_value={'error': False})
    def test_changed_ownership_is_never_stopped_or_removed(self, http):
        self.env.provision()
        cid = self.env.state['containers']['pg']['id']
        self.fake.containers[cid]['Config']['Labels'][OWNER_LABEL] = 'unrelated'
        with self.assertRaises(WorkflowError):
            self.env.cleanup()
        self.assertIn(cid, self.fake.containers)
        self.assertTrue(self.fake.containers[cid]['State']['Running'])

    @patch('tools.ingestion.docker_environment.request', return_value={'error': False})
    def test_failure_retains_resource_identity_and_diagnostics(self, http):
        self.fake.fail_start = True
        with self.assertRaises(WorkflowError):
            self.env.provision()
        state = json.loads(self.env.state_path.read_text())
        self.assertTrue(state['containers'])
        self.assertTrue(list((self.env.output / 'logs').glob('*.log')))

    @patch('tools.ingestion.docker_environment.request', return_value={'error': False})
    def test_neo4j_readiness_uses_system_commands_and_registers_only_generated_database(self, http):
        self.env.provision()
        queries = []
        def administration(url, username, password, body=None):
            query = body['statements'][0]['statement']
            queries.append(query)
            if '/db/system/' in url and query == 'RETURN 1':
                raise WorkflowError('RETURN is not a system command')
            return {'results': [{'data': []}], 'errors': []}
        with patch('tools.ingestion.docker_environment.request', side_effect=administration), \
             patch('tools.ingestion.docker_environment.wait_for', side_effect=lambda check, seconds: check()):
            self.env.neo_start()
        self.assertTrue(any(q.startswith('CREATE DATABASE `' + self.env.database + '` IF NOT EXISTS') for q in queries))
        self.assertTrue(queries[0].startswith('SHOW DATABASES'))

    @patch('tools.ingestion.docker_environment.request', return_value={'error': False})
    def test_interrupted_admin_creation_is_discovered_by_ownership_for_cleanup(self, http):
        self.env.provision()
        cid = 'e' * 64
        self.fake.containers[cid] = {'Id': cid, 'Name': '/' + self.env.generation + '-admin',
            'Config': {'Labels': {OWNER_LABEL: self.env.generation}}, 'State': {'Running': True}}
        self.env.cleanup()
        self.assertFalse(self.fake.containers)

    @patch('tools.ingestion.docker_environment.request', return_value={'error': False})
    def test_foreign_network_prevents_every_cleanup_mutation(self, http):
        self.env.provision()
        self.fake.networks[self.env.state['network']['id']]['Labels'][OWNER_LABEL] = 'foreign'
        before = len(self.fake.calls)
        with self.assertRaises(WorkflowError):
            self.env.cleanup()
        self.assertFalse(any(argv[1] in ('stop', 'rm') for argv, _ in self.fake.calls[before:]))

    def test_prepare_failure_never_provisions_and_cannot_be_retried(self):
        binary = os.environ.get('POLYGRAPH_BULK_IMPORT')
        if not binary:
            self.skipTest('CTest supplies the executable')
        from tools.ingestion import docker_load
        output = self.env.output / 'failed-driver'
        argv = ['docker_load.py', 'load', '--binary', binary, '--adapter', 'ldbc-snb', '--input', 'missing-dataset',
                '--placement-config', str(Path(__file__).resolve().parents[2] / 'tests/fixtures/ingestion/placement.json'),
                '--docker', '--output-dir', str(output), '--arango-concurrency', '2', '--arango-batch-mb', '1',
                '--arango-buffer-mb', '16']
        def create(path, generation, **kwargs):
            return DockerEnvironment(path, generation, runner=self.fake)
        with patch.object(sys, 'argv', argv), patch.object(docker_load, 'DockerEnvironment', side_effect=create), \
             patch.object(docker_load.signal, 'signal'):
            with self.assertRaisesRegex(WorkflowError, 'prepare failed'):
                docker_load.main()
            state = json.loads((output / 'environment.json').read_text())
            self.assertEqual(state['status'], 'failed')
            self.assertFalse(state['containers'])
            target = json.loads((output / 'targets.json').read_text())['targets']['ArangoDB']
            self.assertEqual(target['import_concurrency'], 2)
            self.assertEqual(target['import_batch_mb'], 1)
            self.assertEqual(target['import_buffer_mb'], 16)
            with self.assertRaisesRegex(WorkflowError, 'fresh output'):
                docker_load.main()
        self.assertFalse(any(argv[1] in ('create', 'start') for argv, _ in self.fake.calls))

    def test_interrupted_network_creation_is_recovered_without_touching_foreign_networks(self):
        cid = 'c' * 64
        self.fake.networks[cid] = {'Id': cid, 'Name': self.env.generation + '-network', 'Labels': {OWNER_LABEL: self.env.generation}}
        foreign = {'Id': 'd' * 64, 'Name': 'unrelated', 'Labels': {OWNER_LABEL: 'other'}}
        self.fake.networks[foreign['Id']] = foreign
        self.env.cleanup()
        self.assertEqual(self.fake.networks, {foreign['Id']: foreign})

    def test_cancellation_is_not_retried_as_a_readiness_failure(self):
        from tools.ingestion.docker_environment import wait_for
        def interrupted():
            raise WorkflowInterrupted('cancelled')
        with self.assertRaises(WorkflowInterrupted):
            wait_for(interrupted, 1)

    def test_stage_interrupt_terminates_the_entire_subprocess_group(self):
        from tools.ingestion.docker_load import stage
        with patch('tools.ingestion.docker_load.subprocess.Popen') as spawn, \
             patch('tools.ingestion.docker_load.os.killpg') as killpg:
            process = spawn.return_value.__enter__.return_value
            process.pid = 12345
            process.wait.return_value = -15
            process.communicate.side_effect = WorkflowInterrupted('cancelled')
            with self.assertRaises(WorkflowInterrupted):
                stage(self.env, 'cancelled-stage', ['dummy-importer'])
            self.assertTrue(spawn.call_args.kwargs['start_new_session'])
            killpg.assert_called()
            self.assertEqual(killpg.call_args.args[0], 12345)

    def test_real_cancelled_stage_cannot_leave_a_mutating_grandchild(self):
        root = self.env.output / 'process-test'; root.mkdir(); (root / 'logs').mkdir()
        marker, ready = root / 'late-mutation', root / 'ready'
        grandchild = f"import time;from pathlib import Path;time.sleep(0.5);Path({str(marker)!r}).write_text('unexpected')"
        worker = f"import subprocess,sys,time;from pathlib import Path;subprocess.Popen([sys.executable,'-c',{grandchild!r}]);Path({str(ready)!r}).write_text('ready');time.sleep(10)"
        outer = f"""import os,signal,sys
from pathlib import Path
from types import SimpleNamespace
from tools.ingestion.docker_load import stage
from tools.ingestion.docker_environment import WorkflowInterrupted
def cancel(signum,frame): raise WorkflowInterrupted('cancelled')
signal.signal(signal.SIGTERM,cancel)
env=SimpleNamespace(output=Path({str(root)!r}),env=os.environ.copy(),state={{}},save=lambda:None)
try: stage(env,'child',[sys.executable,'-c',{worker!r}])
except WorkflowInterrupted: sys.exit(42)
"""
        with subprocess.Popen([sys.executable, '-c', outer], stdout=subprocess.PIPE, stderr=subprocess.PIPE) as parent:
            deadline = time.monotonic() + 5
            while not ready.exists() and time.monotonic() < deadline:
                if parent.poll() is not None:
                    self.fail(parent.communicate()[1].decode())
                time.sleep(0.01)
            self.assertTrue(ready.exists())
            parent.send_signal(signal.SIGTERM)
            parent.communicate(timeout=5)
            self.assertEqual(parent.returncode, 42)
        time.sleep(0.7)
        self.assertFalse(marker.exists(), 'A descendant mutated state after cancellation/cleanup')

    def test_native_heterogeneous_edge_types_do_not_require_a_bfs_schema(self):
        binary = os.environ.get('POLYGRAPH_BULK_IMPORT')
        if not binary:
            self.skipTest('CTest supplies the executable')
        from tools.ingestion.docker_load import prepare_runtime
        fixture = Path(__file__).resolve().parents[2] / 'tests/fixtures/ingestion'
        records = [json.loads(s) for s in (fixture / 'canonical.jsonl').read_text().splitlines()]
        for record in records:
            if record.get('kind') == 'relationship': record['type'] = 'CONNECTED_TO'
        source, placement = self.env.output / 'graph.jsonl', self.env.output / 'placement.json'
        source.write_text(''.join(json.dumps(r) + '\n' for r in records))
        config = json.loads((fixture / 'placement.json').read_text()); config['relationshipSources'] = {}
        placement.write_text(json.dumps(config))
        subprocess.run([binary, 'prepare', '--adapter', 'polygraph-jsonl', '--input', str(source), '--placement-config', str(placement),
                        '--output-dir', str(self.env.artifacts)], check=True, capture_output=True)
        subprocess.run([binary, 'verify', '--manifest', str(self.env.artifacts / 'manifest.json'), '--strict'], check=True, capture_output=True)
        manifest = json.loads((self.env.artifacts / 'manifest.json').read_text())
        self.assertIsNone(prepare_runtime(self.env, manifest))
        self.assertFalse(json.loads((self.env.output / 'runtime-bootstrap.json').read_text())['schema_available'])
        self.assertTrue(self.env.runtime_config_path.is_file())
        with self.assertRaisesRegex(WorkflowError, 'multiple endpoint type pairs'):
            prepare_runtime(self.env, manifest, require_bfs=True)
        self.assertFalse(self.fake.containers)

    def test_read_only_validation_can_be_repeated_without_overwriting_logs(self):
        from tools.ingestion.docker_load import stage
        for _ in range(2):
            stage(self.env, 'read-only-check', [sys.executable, '-c', 'print("checked")'])
        logs = list((self.env.output / 'logs').glob('read-only-check*.stdout.log'))
        self.assertEqual(len(logs), 2)
        self.assertTrue(all(p.read_text().strip() == 'checked' for p in logs))

    def test_runtime_configuration_preserves_supported_backend_aliases(self):
        path = self.env.runtime_config(['Neo4j', 'Arango', 'Postgres'])
        config = json.loads(path.read_text())
        self.assertEqual(config['idRouting'], ['Neo4J', 'ArangoDB', 'PostgreSQL'])
        self.assertEqual([entry['id'] for entry in config['backends']], config['idRouting'])

    def test_import_only_summary_never_claims_verified(self):
        from tools.ingestion.docker_load import summary
        manifest = {'counts': {'nodes': 3, 'edges': 3}, 'backends': {
            'Neo4J': {'counts': {'nodes': 1, 'edges': 2, 'proxies': 1}}}}
        report = summary(self.env, manifest, 4.5, True, verification='skipped')
        self.assertEqual(report['state'], 'imported')
        self.assertEqual(report['verification'], 'skipped')
        self.assertEqual(report['duration_seconds'], 4.5)
        self.assertTrue(report['services_running'])

    @patch('tools.ingestion.docker_environment.request', return_value={'error': False})
    def test_separate_verification_preserves_import_journal_and_running_services(self, http):
        from tools.ingestion.docker_load import verify_existing
        self.env.provision()
        self.fake.containers[self.env.state['containers']['neo']['id']]['State']['Running'] = True
        journal = self.env.artifacts / 'execution-state.json'
        manifest = {'generation_id': 'unit-generation', 'backends': {name: {} for name in self.env.targets['targets']}}
        (self.env.artifacts / 'manifest.json').write_text(json.dumps(manifest))
        target_hash = hashlib.sha256(json.dumps(self.env.targets, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        journal.write_text(json.dumps({'state': 'imported', 'generation_id': 'unit-generation', 'target_sha256': target_hash,
            'backends': {name: {'state': 'imported'} for name in manifest['backends']}}))
        original = journal.read_bytes()
        engine = self.env.output / 'verify-engine'
        engine.write_text(f'#!{sys.executable}\n' + """import json,os,sys
from pathlib import Path
assert sys.argv[1]=='verify-live', 'verification must never invoke an importer'
assert os.environ['POLYGRAPH_LOAD_PASSWORD']
p=Path(sys.argv[sys.argv.index('--manifest')+1]).parent
(p/'live-verification.json').write_text(json.dumps({'state':'verified','backends':{}}))
""")
        engine.chmod(0o700)
        before = len(self.fake.calls)
        report = verify_existing(self.env, engine)
        self.assertEqual(report['state'], 'verified')
        self.assertGreater(report['duration_seconds'], 0)
        self.assertEqual(journal.read_bytes(), original)
        self.assertTrue(all(c['State']['Running'] for c in self.fake.containers.values()))
        self.assertFalse(any(argv[1] in ('create', 'start', 'stop', 'rm', 'run') for argv, _ in self.fake.calls[before:]))
        self.env.targets['targets']['PostgreSQL']['database'] = 'different-target'
        self.env.target_path.write_text(json.dumps(self.env.targets))
        try:
            with self.assertRaisesRegex(WorkflowError, 'different generation/target'):
                verify_existing(self.env, engine)
        finally:
            self.env.targets['targets']['PostgreSQL']['database'] = self.env.database
            self.env.target_path.write_text(json.dumps(self.env.targets))
        (self.env.output / 'load-result.json').write_text(json.dumps({'state': 'verified', 'verification': 'full',
            'counts_source': 'live-verification', 'bfs': {'verified': True}, 'duration_seconds': 4.5}))
        engine.write_text(f'#!{sys.executable}\nimport sys\nprint("canonical ID mismatch",file=sys.stderr)\nsys.exit(7)\n')
        with self.assertRaisesRegex(WorkflowError, 'canonical ID mismatch'):
            verify_existing(self.env, engine)
        self.assertEqual(json.loads((self.env.output / 'verification-result.json').read_text())['state'], 'failed')
        failed = json.loads((self.env.output / 'load-result.json').read_text())
        self.assertIsNone(failed['bfs'])
        self.assertEqual(failed['counts_source'], 'manifest')
        self.assertEqual(failed['duration_seconds'], 4.5)
        self.assertEqual(journal.read_bytes(), original)
        self.assertTrue(all(c['State']['Running'] for c in self.fake.containers.values()))

    def test_repeated_cancellation_cannot_interrupt_cleanup(self):
        from tools.ingestion.docker_load import cancellation_scope
        previous = signal.getsignal(signal.SIGINT)
        with cancellation_scope():
            with self.assertRaises(WorkflowInterrupted):
                signal.raise_signal(signal.SIGINT)
            signal.raise_signal(signal.SIGINT)
            signal.raise_signal(signal.SIGTERM)
        self.assertEqual(signal.getsignal(signal.SIGINT), previous)

    def test_cli_rejects_skip_verification_with_bfs_before_creating_output(self):
        binary = os.environ.get('POLYGRAPH_BULK_IMPORT')
        if not binary:
            self.skipTest('CTest supplies the executable')
        output = self.env.output / 'incompatible'
        result = subprocess.run([binary, 'load', '--adapter', 'ldbc-snb', '--input', 'missing',
            '--placement-config', 'missing', '--docker', '--skip-verification', '--verify-bfs', '--output-dir', str(output)],
            capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('cannot be combined', result.stderr)
        self.assertFalse(output.exists())

    def test_invalid_arango_tuning_fails_before_creating_an_environment(self):
        binary = os.environ.get('POLYGRAPH_BULK_IMPORT')
        if not binary:
            self.skipTest('CTest supplies the executable')
        for tuning in (['--arango-concurrency', '0'], ['--arango-concurrency', '33'],
                       ['--arango-batch-mb', '0'], ['--arango-buffer-mb', '1', '--arango-batch-mb', '8']):
            output = self.env.output / 'invalid-tuning'
            result = subprocess.run([binary, 'load', '--adapter', 'ldbc-snb', '--input', 'missing',
                '--placement-config', 'missing', '--docker', '--output-dir', str(output), *tuning],
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Arango import settings', result.stderr)
            self.assertFalse(output.exists())

    def test_partition_options_are_forwarded_to_offline_prepare_before_provisioning(self):
        binary = os.environ.get('POLYGRAPH_BULK_IMPORT')
        if not binary:
            self.skipTest('CTest supplies the executable')
        from tools.ingestion import docker_load
        output = self.env.output / 'partition-forwarding'
        argv = ['docker_load.py', 'load', '--binary', binary, '--adapter', 'ldbc-snb', '--input', 'missing',
                '--placement-config', 'placement.json', '--docker', '--output-dir', str(output),
                '--partitioner', 'kaminpar', '--partitions', '8', '--partition-threads', '2',
                '--partition-imbalance', '0.05', '--partition-preset', 'eco', '--seed', '17',
                '--partition-map', 'mapping.json']
        def create(path, generation, **kwargs):
            return DockerEnvironment(path, generation, runner=self.fake)
        with patch.object(sys, 'argv', argv), patch.object(docker_load, 'DockerEnvironment', side_effect=create), \
             patch.object(docker_load, 'stage', side_effect=WorkflowError('prepare stopped')) as stage, \
             patch.object(docker_load.signal, 'signal'):
            with self.assertRaisesRegex(WorkflowError, 'prepare stopped'):
                docker_load.main()
        command = list(map(str, stage.call_args.args[2]))
        for key, value in [('--partitioner', 'kaminpar'), ('--partitions', '8'), ('--partition-threads', '2'),
                           ('--partition-imbalance', '0.05'), ('--partition-preset', 'eco'), ('--seed', '17')]:
            self.assertEqual(command[command.index(key) + 1], value)
        self.assertEqual(Path(command[command.index('--partition-map') + 1]), Path('mapping.json').resolve())
        self.assertFalse(any(argv[1] in ('create', 'start') for argv, _ in self.fake.calls))

    def test_invalid_partition_controls_fail_before_creating_an_environment(self):
        from tools.ingestion import docker_load
        binary = os.environ.get('POLYGRAPH_BULK_IMPORT', '/missing/binary')
        for flags in (['--partitioner', 'kaminpar', '--partitions', '0'], ['--partitions', '2'],
                      ['--partitioner', 'kaminpar', '--partitions', '2', '--partition-threads', '0'],
                      ['--partitioner', 'kaminpar', '--partitions', '2', '--partition-imbalance', 'nan'],
                      ['--partitioner', 'kaminpar', '--partitions', '2', '--partition-file', 'external.jsonl'],
                      ['--partition-map', 'map.json']):
            argv = ['docker_load.py', 'load', '--binary', binary, '--adapter', 'ldbc-snb', '--input', 'missing',
                    '--placement-config', 'missing', '--docker', *flags]
            with self.subTest(flags=flags), patch.object(sys, 'argv', argv), \
                 patch.object(docker_load, 'DockerEnvironment') as environment:
                with self.assertRaises(SystemExit) as raised:
                    docker_load.main()
                self.assertEqual(raised.exception.code, 2)
                environment.assert_not_called()

    def test_existing_generation_cannot_be_reused(self):
        with self.assertRaises(WorkflowError):
            DockerEnvironment(self.env.output, self.env.generation, runner=self.fake)

    def test_cli_load_requires_explicit_docker_and_documents_flags(self):
        binary = os.environ.get('POLYGRAPH_BULK_IMPORT')
        if not binary:
            self.skipTest('CTest supplies the executable')
        help_text = subprocess.check_output([binary, '--help'], text=True)
        for option in ('load', '--docker', '--keep-running', '--generation-name', '--verify-bfs'):
            self.assertIn(option, help_text)
        result = subprocess.run([binary, 'load', '--adapter', 'ldbc-snb', '--input', 'missing', '--placement-config', 'missing'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('--docker', result.stderr)


if __name__ == '__main__':
    unittest.main()
