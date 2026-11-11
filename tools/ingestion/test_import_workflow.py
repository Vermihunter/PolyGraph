"""Exercise the generic import command against the real native engine."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ImportWorkflowCliTest(unittest.TestCase):
    def setUp(self):
        binary = os.environ.get('POLYGRAPH_BULK_IMPORT')
        if not binary:
            self.skipTest('CTest supplies POLYGRAPH_BULK_IMPORT')
        self.binary = str(Path(binary).resolve())
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.domain = self.root / 'domain'
        self.domain.mkdir()
        example = ROOT / 'docs/examples/ingestion/logical-graph'
        for name in ('graph.jsonl', 'schema.json', 'placement.json', 'backends.json'):
            shutil.copyfile(example / name, self.domain / name)
        self.manifest = self.domain / 'domain.json'
        self.document = {
            'format': 'polygraph.domain', 'version': 1, 'id': 'import-cli',
            'paths': {'schema': 'schema.json', 'data': 'graph.jsonl', 'backendConfig': 'backends.json',
                      'placementConfig': 'placement.json', 'ingestionArtifacts': '../artifacts'},
            'ingestion': {'adapter': 'polygraph-jsonl'}}
        self.save()
        self.environment = dict(os.environ)
        self.environment['PYTHONDONTWRITEBYTECODE'] = '1'
        # Any unexpected lifecycle command leaves evidence; no real service is used.
        traps = self.root / 'traps'
        traps.mkdir()
        self.marker = self.root / 'unexpected-service-command'
        for executable in ('docker', 'docker-compose', 'podman', 'neo4j-admin'):
            path = traps / executable
            path.write_text('#!/bin/sh\ntouch "' + str(self.marker) + '"\nexit 99\n')
            path.chmod(0o755)
        self.environment['PATH'] = str(traps) + os.pathsep + self.environment.get('PATH', '')

    def save(self):
        self.manifest.write_text(json.dumps(self.document))

    def cli(self, *options, python=False):
        prefix = ([sys.executable, '-B', str(ROOT / 'tools/polygraph.py'), 'import', '--binary', self.binary]
                  if python else [self.binary, 'import'])
        return subprocess.run([*prefix, '--graph-manifest', str(self.manifest), *options],
                              cwd=self.root, env=self.environment, capture_output=True, text=True)

    def snapshot(self):
        return {str(p): (p.stat().st_mtime_ns, p.read_bytes() if p.is_file() else None)
                for p in [self.root, *self.root.rglob('*')]}

    def test_default_prepares_both_supported_adapters_with_existing_writers(self):
        for adapter in ('polygraph-jsonl', 'ldbc-snb'):
            with self.subTest(adapter=adapter):
                self.document['ingestion']['adapter'] = adapter
                output = self.root / adapter
                self.document['paths']['ingestionArtifacts'] = str(output)
                if adapter == 'ldbc-snb':
                    fixture = ROOT / 'tests/fixtures/ingestion'
                    self.document['paths'].update(sourceData=str(fixture / 'raw'),
                        schema=str(ROOT / 'docs/examples/ingestion/ldbc.schema.json'),
                        placementConfig=str(fixture / 'placement.json'))
                self.save()
                result = self.cli()
                self.assertEqual(result.returncode, 0, result.stderr)
                report = json.loads(result.stdout)
                self.assertEqual(report['state'], 'prepared')
                self.assertEqual(report['adapter'], adapter)
                self.assertEqual(report['graph'], 'import-cli')
                self.assertEqual(report['output_dir'], str(output))
                self.assertEqual(Path(report['manifest']), output / 'manifest.json')
                actual = json.loads((output / 'manifest.json').read_text())
                self.assertEqual(report['counts'], actual['counts'])
                self.assertEqual(set(report['backends']), {'Neo4J', 'ArangoDB', 'PostgreSQL'})
                verified = subprocess.run([self.binary, 'verify', '--manifest', report['manifest'], '--strict'],
                                          capture_output=True, text=True)
                self.assertEqual(verified.returncode, 0, verified.stderr)
                again = self.cli()
                self.assertEqual(again.returncode, 0, again.stderr)
                self.assertEqual(json.loads(again.stdout)['generation_id'], report['generation_id'])
                self.assertFalse((output / 'execution-state.json').exists())
        self.assertFalse(self.marker.exists())

    def test_dry_run_is_read_only_for_native_and_python(self):
        for python in (False, True):
            with self.subTest(python=python):
                before = self.snapshot()
                result = self.cli('--dry-run', python=python)
                self.assertEqual(result.returncode, 0, result.stderr)
                report = json.loads(result.stdout)
                self.assertEqual(report['state'], 'valid')
                self.assertEqual(report['adapter'], 'polygraph-jsonl')
                self.assertIn('not_checked', report)
                self.assertEqual(before, self.snapshot())

    def test_invalid_modes_and_execution_without_consent_fail_before_writes(self):
        for flags in (('--execute',), ('--confirm-create',), ('--dry-run', '--execute', '--confirm-create'),
                      ('--dry-run', '--confirm-create'), ('--concurrency', '2'),
                      ('--execute', '--confirm-create', '--concurrency', '0'),
                      ('--execute', '--confirm-create', '--concurrency', '65')):
            with self.subTest(flags=flags):
                before = self.snapshot()
                result = self.cli(*flags)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('phase=arguments', result.stderr)
                self.assertEqual(before, self.snapshot())

    def test_explicit_execution_requires_targets_before_preparing(self):
        before = self.snapshot()
        result = self.cli('--execute', '--confirm-create')
        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertIn('adapter=polygraph-jsonl', result.stderr)
        self.assertIn('phase=validate', result.stderr)
        self.assertIn('targetConfig', result.stderr)
        self.assertEqual(before, self.snapshot())

    def test_source_error_reports_adapter_phase_and_existing_file_record_context(self):
        source = self.domain / 'graph.jsonl'
        source.write_text(source.read_text() + '{invalid source record}\n')
        result = self.cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('adapter=polygraph-jsonl', result.stderr)
        self.assertIn('phase=prepare', result.stderr)
        self.assertIn('graph.jsonl', result.stderr)
        self.assertFalse((self.root / 'artifacts/manifest.json').exists())
        self.assertFalse(self.marker.exists())

    def test_python_prepares_by_forwarding_to_native_without_build_or_services(self):
        result = self.cli(python=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['counts'], {'nodes': 4, 'edges': 4})
        self.assertEqual(report['state'], 'prepared')
        self.assertFalse(self.marker.exists())

    def test_unknown_adapter_has_configuration_phase_context(self):
        self.document['ingestion']['adapter'] = 'missing-adapter'
        self.save()
        before = self.snapshot()
        result = self.cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('phase=configuration', result.stderr)
        self.assertIn('missing-adapter', result.stderr)
        self.assertEqual(before, self.snapshot())

    def test_prepare_ignores_execution_commands_and_does_not_touch_existing_data(self):
        targets = json.loads((ROOT / 'docs/examples/ingestion/targets.example.json').read_text())
        targets['targets']['Neo4J'].update(
            runner=[str(self.root / 'traps/neo4j-admin')],
            preflight_command=[str(self.root / 'traps/docker')],
            start_command=[str(self.root / 'traps/docker-compose')],
            password_env='POLYGRAPH_TEST_UNSET_IMPORT_SECRET')
        self.environment.pop('POLYGRAPH_TEST_UNSET_IMPORT_SECRET', None)
        (self.domain / 'targets.json').write_text(json.dumps(targets))
        self.document['paths']['targetConfig'] = 'targets.json'
        self.save()
        existing = self.root / 'existing-database-data'
        existing.write_bytes(b'pre-existing data must remain untouched')
        before = (existing.stat().st_mtime_ns, existing.read_bytes())
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['state'], 'prepared')
        self.assertEqual(before, (existing.stat().st_mtime_ns, existing.read_bytes()))
        self.assertFalse(self.marker.exists())

    def test_confirmed_execution_reaches_native_backend_preflight_and_preserves_failure(self):
        fixture = ROOT / 'tests/fixtures/ingestion'
        self.document['ingestion']['adapter'] = 'ldbc-snb'
        self.document['paths'].update(sourceData=str(fixture / 'raw'),
            schema=str(ROOT / 'docs/examples/ingestion/ldbc.schema.json'), targetConfig='targets.json')
        (self.domain / 'placement.json').write_text(json.dumps({
            'storageBackends': ['PostgreSQL'], 'defaultNodeSource': 'PostgreSQL',
            'defaultRelationshipSource': 'PostgreSQL'}))
        # A nonexistent local socket cannot connect to any real database.
        (self.domain / 'targets.json').write_text(json.dumps({'targets': {'PostgreSQL': {
            'database': 'import_test_pg', 'host': str(self.root / 'no-postgres-socket')}}}))
        self.save()
        check = subprocess.run([self.binary, 'validate', '--graph-manifest', str(self.manifest), '--execution'],
                               env=self.environment, capture_output=True, text=True)
        if 'native executor is unavailable' in check.stderr:
            self.skipTest('Optional PostgreSQL native executor not compiled')
        self.assertEqual(check.returncode, 0, check.stderr)
        result = self.cli('--execute', '--confirm-create', python=True)
        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertIn('adapter=ldbc-snb phase=execute', result.stderr)
        self.assertIn('backend=PostgreSQL phase=preflight', result.stderr)
        self.assertIn('Cannot connect to named PostgreSQL target', result.stderr)
        self.assertTrue((self.root / 'artifacts/manifest.json').is_file())
        self.assertFalse((self.root / 'artifacts/live-verification.json').exists())
        self.assertFalse(self.marker.exists())

    def test_python_dry_run_does_not_create_bytecode_in_fresh_checkout(self):
        tools = self.root / 'fresh/tools'
        tools.mkdir(parents=True)
        for name in ('polygraph.py', '__init__.py'):
            shutil.copyfile(ROOT / 'tools' / name, tools / name)
        for package in ('graphio', 'ldbc_snb'):
            shutil.copytree(ROOT / 'tools' / package, tools / package,
                            ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '.datagen'))
        environment = dict(self.environment)
        environment.pop('PYTHONDONTWRITEBYTECODE', None)
        environment.pop('PYTHONPYCACHEPREFIX', None)
        before = self.snapshot()
        result = subprocess.run([sys.executable, str(tools / 'polygraph.py'), 'import', '--dry-run',
                                 '--graph-manifest', str(self.manifest), '--binary', self.binary],
                                cwd=self.root, env=environment, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(before, self.snapshot())


if __name__ == '__main__':
    unittest.main()
