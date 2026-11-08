"""Native graph-manifest CLI regressions; preflight must not mutate its workspace."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from tools.graphio.domain import load_domain_manifest

ROOT = Path(__file__).resolve().parents[2]


class GraphManifestCliTest(unittest.TestCase):
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
        for name in ('graph.jsonl', 'schema.json', 'placement.json'):
            shutil.copyfile(example / name, self.domain / name)
        (self.domain / 'backends.json').write_text(json.dumps({'backends': [
            {'kind': 'Neo4j'}, {'kind': 'ArangoDB'}, {'kind': 'PostgreSQL'}]}))
        self.manifest = self.domain / 'domain.json'
        self.document = {
            'format': 'polygraph.domain', 'version': 1, 'id': 'manifest-cli',
            'paths': {'schema': 'schema.json', 'data': 'graph.jsonl', 'backendConfig': 'backends.json',
                      'placementConfig': 'placement.json', 'ingestionArtifacts': '../artifacts'},
            'defaults': {'bfs': {'startUuid': '281474976710657'}},
            'ingestion': {'adapter': 'polygraph-jsonl', 'identity': {'strategy': 'preserve-canonical-v2'}}}
        self.save()

    def save(self):
        self.manifest.write_text(json.dumps(self.document))

    def run_cli(self, *args):
        return subprocess.run([self.binary, *args], cwd=self.root, capture_output=True, text=True)

    def snapshot(self):
        return {str(p): (p.stat().st_mtime_ns, p.read_bytes() if p.is_file() else None)
                for p in [self.root, *self.root.rglob('*')]}

    def test_validate_is_read_only_and_relative_paths_ignore_working_directory(self):
        before = self.snapshot()
        result = self.run_cli('validate', '--graph-manifest', str(self.manifest))
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['graph'], 'manifest-cli')
        self.assertEqual(report['state'], 'valid')
        self.assertEqual(report['output_dir'], str(self.root / 'artifacts'))
        self.assertIn('record uniqueness/properties/endpoint closure', report['not_checked'])
        self.assertEqual(before, self.snapshot())

    def test_prepare_uses_same_manifest_and_existing_native_writers(self):
        result = self.run_cli('prepare', '--graph-manifest', str(self.manifest))
        self.assertEqual(result.returncode, 0, result.stderr)
        prepared = self.root / 'artifacts/manifest.json'
        actual = json.loads(prepared.read_text())
        self.assertEqual(actual['counts'], {'nodes': 4, 'edges': 4})
        self.assertEqual(set(actual['backends']), {'Neo4J', 'ArangoDB', 'PostgreSQL'})
        self.assertEqual(self.run_cli('verify', '--manifest', str(prepared), '--strict').returncode, 0)

    def test_conflicting_flags_and_missing_execution_targets_fail_without_mutation(self):
        before = self.snapshot()
        result = self.run_cli('prepare', '--graph-manifest', str(self.manifest), '--threads', '2')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('graph.options:', result.stderr)
        result = self.run_cli('validate', '--graph-manifest', str(self.manifest), '--execution')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('graph.execution:', result.stderr)
        self.assertEqual(before, self.snapshot())

    def test_existing_python_domain_reader_accepts_extended_and_legacy_manifest(self):
        extended = load_domain_manifest(self.manifest)
        self.document.pop('ingestion')
        self.save()
        legacy = load_domain_manifest(self.manifest)
        self.assertEqual(extended, legacy)

    def test_python_entrypoint_delegates_to_native_validation_without_building(self):
        before = self.snapshot()
        result = subprocess.run([sys.executable, str(ROOT / 'tools/polygraph.py'), 'validate',
                                 '--graph-manifest', str(self.manifest), '--binary', self.binary],
                                cwd=self.root, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['state'], 'valid')
        self.assertEqual(before, self.snapshot())

    def test_checked_in_ldbc_and_jsonl_manifests_validate_without_services(self):
        for relative, adapter in [('docs/examples/ingestion/ldbc.domain.json', 'ldbc-snb'),
                                  ('docs/examples/ingestion/logical-graph/domain.json', 'polygraph-jsonl')]:
            with self.subTest(adapter=adapter):
                result = self.run_cli('validate', '--graph-manifest', str(ROOT / relative))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout)['adapter'], adapter)

    def test_python_validation_does_not_create_bytecode_in_a_fresh_checkout(self):
        tools = self.root / 'fresh/tools'
        tools.mkdir(parents=True)
        for name in ('polygraph.py', '__init__.py'):
            shutil.copyfile(ROOT / 'tools' / name, tools / name)
        for package in ('graphio', 'ldbc_snb'):
            shutil.copytree(ROOT / 'tools' / package, tools / package,
                            ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '.datagen'))
        before = self.snapshot()
        environment = dict(os.environ)
        environment.pop('PYTHONDONTWRITEBYTECODE', None)
        environment.pop('PYTHONPYCACHEPREFIX', None)
        result = subprocess.run([sys.executable, str(tools / 'polygraph.py'), 'validate',
                                 '--graph-manifest', str(self.manifest), '--binary', self.binary],
                                cwd=self.root, env=environment, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        after = self.snapshot()
        changed = sorted(p for p in before.keys() | after.keys() if before.get(p) != after.get(p))
        self.assertEqual(changed, [], 'Validation changed the fresh checkout')


if __name__ == '__main__':
    unittest.main()
