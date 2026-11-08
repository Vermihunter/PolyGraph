from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from tools import polygraph
from tools.graphio.domain import load_domain_manifest, resolve_domain

ROOT = Path(__file__).resolve().parents[1]


class GraphToolsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='polygraph-external-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.domain = self.root / 'arbitrary graph'
        self.domain.mkdir()
        for name in ('schema.json', 'graph.jsonl', 'backends.json', 'placement.json'):
            shutil.copyfile(ROOT / 'docs/examples/ingestion/logical-graph' / name, self.domain / name)
        self.document = {'format': 'polygraph.domain', 'version': 1, 'id': 'external-inventory',
            'paths': {'schema': 'schema.json', 'data': 'graph.jsonl', 'backendConfig': 'backends.json',
                      'placementConfig': 'placement.json', 'ingestionArtifacts': '../artifacts'},
            'defaults': {'bfs': {'startUuid': '281474976710657'}}, 'ingestion': {'adapter': 'polygraph-jsonl'}}
        self.manifest = self.domain / 'domain.json'
        self.save()

    def save(self):
        self.manifest.write_text(json.dumps(self.document))

    def test_one_resolver_accepts_external_directory_id_and_file(self):
        from tools.graphio.domain import resolve_graph_manifest
        for reference in (self.domain, self.manifest, 'external-inventory'):
            self.assertEqual(resolve_graph_manifest(reference, self.root), self.manifest)
            self.assertEqual(resolve_domain(reference, self.root).paths.data, self.domain / 'graph.jsonl')
        duplicate = self.root / 'duplicate'
        shutil.copytree(self.domain, duplicate)
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            resolve_graph_manifest('external-inventory', self.root)

    def test_import_only_and_typed_root_domains_do_not_require_numeric_defaults(self):
        self.document.pop('defaults')
        self.save()
        self.assertIsNone(load_domain_manifest(self.manifest).start_uuid)
        selector = {'namespace': 'inventory', 'type': 'Device', 'idType': 'string', 'id': 'sensor/7'}
        self.document['defaults'] = {'bfs': {'startSource': selector}}
        self.save()
        graph = load_domain_manifest(self.manifest)
        self.assertIsNone(graph.start_uuid)
        self.assertEqual(graph.start_source, selector)
        self.assertEqual(graph.paths.ingestion_artifacts, self.root / 'artifacts')

    def test_generic_cli_aliases_preserve_legacy_positional_selection(self):
        parser = polygraph.build_parser()
        for command in (['run'], ['load'], ['cache', 'build', '--config', str(self.domain / 'placement.json')]):
            with self.subTest(command=command):
                args = parser.parse_args([*command, '--graph-manifest', str(self.domain), '--data-root', str(self.root)])
                self.assertEqual(polygraph.resolve_graph_config(args).paths.manifest, self.manifest)
                args = parser.parse_args([*command[:2], 'external-inventory', *command[2:], '--data-root', str(self.root)])
                self.assertEqual(polygraph.resolve_graph_config(args).paths.manifest, self.manifest)
        args = parser.parse_args(['run', str(self.manifest), '--graph-manifest', str(self.manifest)])
        with self.assertRaisesRegex(ValueError, 'either|both|one'):
            polygraph.resolve_graph_config(args)

    def test_run_forwards_typed_roots_and_writes_measurements_outside_repository(self):
        selector = {'namespace': 'inventory', 'type': 'Device', 'idType': 'string', 'id': 'sensor/7'}
        self.document['defaults'] = {'bfs': {'startSource': selector}}
        self.save()
        capture = self.root / 'arguments.json'
        binary = self.root / 'fake-bfs'
        binary.write_text('#!' + sys.executable + '\nimport sys,json,pathlib\n'
            + 'pathlib.Path(' + repr(str(capture)) + ').write_text(json.dumps(sys.argv[1:]))\n'
            + 'out=sys.argv[sys.argv.index("--output-file")+1]\n'
            + 'pathlib.Path(out).write_text("vertex_id;distance\\n281474976710657;0\\n")\n')
        binary.chmod(0o755)
        output = self.root / 'measurements'
        args = polygraph.build_parser().parse_args(['run', '--graph-manifest', str(self.domain),
            '--binary', str(binary), '--output-dir', str(output), '--run-id', 'external-run'])
        args.bfs_args = []
        polygraph.run_domain(args)
        actual = json.loads(capture.read_text())
        self.assertNotIn('--start-uuid', actual)
        self.assertEqual(json.loads(actual[actual.index('--start-source') + 1]), selector)
        self.assertEqual(actual[actual.index('--domain') + 1], str(self.manifest))
        with (output / 'measurements.csv').open() as stream:
            row = next(csv.DictReader(stream))
        self.assertEqual(row['start_uuid'], '')
        self.assertEqual(json.loads(row['start_source']), selector)
        self.assertTrue((output / row['output_csv']).is_file())

    def test_run_missing_or_conflicting_roots_fails_before_outputs(self):
        self.document.pop('defaults')
        self.save()
        output = self.root / 'must-not-exist'
        args = polygraph.build_parser().parse_args(['run', str(self.manifest), '--binary', sys.executable,
                                                   '--output-dir', str(output)])
        args.bfs_args = []
        with self.assertRaisesRegex(ValueError, 'root|start-uuid|start-source'):
            polygraph.run_domain(args)
        self.assertFalse(output.exists())

    def test_benchmark_config_relative_paths_are_opt_in(self):
        from tools.measurement.benchmark_suite import load_suite_config
        suite = self.root / 'suite.json'
        document = {'name': 'external', 'pathBase': 'config', 'datasets': [{'name': 'inventory', 'domain': 'arbitrary graph/domain.json'}],
                    'outputDir': 'results', 'graphDbBinary': 'bin/bfs', 'dockerCompose': 'compose.yml'}
        suite.write_text(json.dumps(document))
        config = load_suite_config(suite)
        self.assertEqual(config.datasets[0].domain, self.manifest)
        self.assertEqual(config.output_dir, self.root / 'results')
        document.pop('pathBase')
        suite.write_text(json.dumps(document))
        self.assertEqual(load_suite_config(suite).output_dir, ROOT / 'results')

    def test_reference_script_uses_external_graph_config(self):
        output = self.root / 'reference.csv'
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/algorithms/bfs.py'),
            '--graph-manifest', str(self.domain), '--output-csv', str(output), '--max-depth', '0'],
            cwd=self.root, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('281474976710657;0', output.read_text())

    def test_legacy_loader_manifest_mode_delegates_to_native_dry_run(self):
        binary = os.environ.get('POLYGRAPH_BULK_IMPORT')
        if not binary:
            self.skipTest('CTest supplies native binary')
        args = [sys.executable, '-B', str(ROOT / 'tools/load_graph.py'), '--graph-manifest', str(self.domain),
                '--binary', binary, '--dry-run']
        result = subprocess.run(args, cwd=self.root, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['adapter'], 'polygraph-jsonl')
        self.assertFalse((self.root / 'artifacts').exists())
        result = subprocess.run([*args, '--reset'], cwd=self.root, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('legacy', result.stderr.lower())
        self.assertFalse((self.root / 'artifacts').exists())

    def test_generic_parser_does_not_require_ldbc_support_package(self):
        tools = self.root / 'fresh/tools'
        tools.mkdir(parents=True)
        for name in ('polygraph.py', '__init__.py'):
            shutil.copyfile(ROOT / 'tools' / name, tools / name)
        shutil.copytree(ROOT / 'tools/graphio', tools / 'graphio', ignore=shutil.ignore_patterns('__pycache__'))
        result = subprocess.run([sys.executable, '-B', str(tools / 'polygraph.py'), 'run', '--help'],
                                cwd=self.root, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--graph-manifest', result.stdout)

    def test_numeric_run_keeps_existing_measurement_columns_and_default_paths(self):
        capture = self.root / 'numeric-arguments.json'
        binary = self.root / 'numeric-bfs'
        binary.write_text('#!' + sys.executable + '\nimport sys,json,pathlib\n'
            + 'pathlib.Path(' + repr(str(capture)) + ').write_text(json.dumps(sys.argv[1:]))\n'
            + 'pathlib.Path(sys.argv[sys.argv.index("--output-file")+1]).write_text("vertex_id;distance\\n281474976710657;0\\n")\n')
        binary.chmod(0o755)
        args = polygraph.build_parser().parse_args(['run', str(self.manifest), '--binary', str(binary), '--run-id', 'numeric'])
        args.bfs_args = []
        with patch.object(polygraph, 'ROOT', self.root / 'project'):
            polygraph.run_domain(args)
        actual = json.loads(capture.read_text())
        self.assertNotIn('--start-source', actual)
        self.assertEqual(actual[actual.index('--start-uuid')+1], '281474976710657')
        output = self.root / 'project/measurements/numeric'
        with (output / 'measurements.csv').open() as stream:
            rows = csv.DictReader(stream)
            self.assertNotIn('start_source', rows.fieldnames)
            row = next(rows)
        self.assertEqual(row['start_uuid'], '281474976710657')
        self.assertEqual(row['output_csv'], 'outputs/281474976710657/1.csv')

    def test_native_load_alias_and_import_resolve_external_id_without_services(self):
        binary = os.environ.get('POLYGRAPH_BULK_IMPORT')
        if not binary:
            self.skipTest('CTest supplies native binary')
        for command in ('import', 'load'):
            result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/polygraph.py'), command,
                '--graph-manifest', 'external-inventory', '--data-root', str(self.root),
                '--binary', binary, '--dry-run'], cwd=self.root, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['graph'], 'external-inventory')
            self.assertFalse((self.root / 'artifacts').exists())

    def test_native_load_alias_rejects_legacy_reset_and_selector_conflicts(self):
        parser = polygraph.build_parser()
        for extra in (['--reset'], ['--batch-size', '3'], ['--only-source', 'Neo4J']):
            args = parser.parse_args(['load', '--graph-manifest', str(self.manifest), *extra])
            with self.assertRaisesRegex(ValueError, 'legacy'):
                polygraph.load_domain_compat(args)
        args = parser.parse_args(['load', str(self.manifest), '--dry-run'])
        with self.assertRaisesRegex(ValueError, 'require.*--graph-manifest'):
            polygraph.load_domain_compat(args)
        self.assertFalse((self.root / 'artifacts').exists())

    def test_external_root_csv_uses_selected_property_and_ldbc_alias_keeps_its_value(self):
        from tools.graphio.domain import write_roots_csv
        from tools.graphio.model import NodeRecord
        node = NodeRecord(uuid=281474976710657, type='Device', source='Neo4J', properties={'external_id':'device/7','ldbc_id':7})
        for property_name, expected in [('external_id', 'device/7'), ('ldbc_id', '7')]:
            output = write_roots_csv(self.root / (property_name + '.csv'), [node], external_id_property=property_name)
            with output.open() as stream:
                row = next(csv.DictReader(stream))
            self.assertEqual(row['external_id'], expected)

    def test_reference_typed_default_has_explicit_native_replacement(self):
        self.document['defaults'] = {'bfs': {'startSource': {'namespace':'n','type':'Device','idType':'string','id':'7'}}}
        self.save()
        output = self.root / 'not-created.csv'
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/algorithms/bfs.py'),
            '--graph-manifest', str(self.domain), '--output-csv', str(output)],
            cwd=self.root, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('polygraph.py run', result.stderr)
        self.assertFalse(output.exists())

    def test_native_alias_dry_runs_leave_fresh_checkout_unchanged_without_ldbc(self):
        binary = os.environ.get('POLYGRAPH_BULK_IMPORT')
        if not binary:
            self.skipTest('CTest supplies native binary')
        tools = self.root / 'fresh/tools'
        tools.mkdir(parents=True)
        for name in ('polygraph.py', 'load_graph.py', '__init__.py'):
            shutil.copyfile(ROOT / 'tools' / name, tools / name)
        shutil.copytree(ROOT / 'tools/graphio', tools / 'graphio', ignore=shutil.ignore_patterns('__pycache__'))
        def snapshot():
            return {str(p): (p.stat().st_mtime_ns, p.read_bytes() if p.is_file() else None)
                    for p in [self.root, *self.root.rglob('*')]}
        environment = dict(os.environ)
        environment.pop('PYTHONDONTWRITEBYTECODE', None)
        environment.pop('PYTHONPYCACHEPREFIX', None)
        for script, prefix in [('polygraph.py', ['load']), ('load_graph.py', [])]:
            before = snapshot()
            result = subprocess.run([sys.executable, str(tools / script), *prefix,
                '--graph-manifest=' + str(self.domain), '--binary', binary, '--dry-run'],
                cwd=self.root, env=environment, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(before, snapshot())

    def test_forwarded_conflicting_or_empty_source_roots_fail_before_measurements(self):
        output = self.root / 'no-measurements'
        for selector, forwarded in [('', []), ('{}', ['--start-uuid', '281474976710657'])]:
            args = polygraph.build_parser().parse_args(['run', str(self.manifest), '--binary', sys.executable,
                '--start-source', selector, '--output-dir', str(output)])
            args.bfs_args = forwarded
            with self.assertRaises(ValueError):
                polygraph.run_domain(args)
            self.assertFalse(output.exists())

    def test_malformed_config_has_a_clear_structural_error(self):
        self.document['paths'].pop('schema')
        self.save()
        with self.assertRaisesRegex(ValueError, r'domain.paths.schema'):
            resolve_domain(self.domain, self.root)

    def test_non_executable_native_binary_fails_before_artifacts(self):
        binary = self.root / 'not-executable'
        binary.write_text('not a native executable')
        args = polygraph.build_parser().parse_args(['import', '--graph-manifest', str(self.domain), '--binary', str(binary)])
        with self.assertRaisesRegex(FileNotFoundError, 'not executable'):
            polygraph.native_graph_command(args, 'import')
        self.assertFalse((self.root / 'artifacts').exists())

    def test_reference_honors_domain_depth_direction_and_explicit_overrides(self):
        a, b = '281474976710657', '281474976710658'
        records = [
            {'kind':'metadata','idRouting':['Neo4J','ArangoDB','PostgreSQL']},
            {'kind':'node','uuid':a,'type':'Device','source':'Neo4J','properties':{}},
            {'kind':'node','uuid':b,'type':'Site','source':'Neo4J','properties':{}},
            {'kind':'relationship','uuid':'281474976710659','type':'INSTALLED_AT','source':'Neo4J','from':a,'to':b,'properties':{}}]
        (self.domain / 'graph.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in records))
        for root, depth, direction, extra, expected in [
                (a, 0, 'directed', [], {a:0}),
                (b, 2, 'directed', [], {b:0}),
                (b, 0, 'directed', ['--max-depth','1','--undirected'], {b:0,a:1}),
                (b, 2, 'undirected', ['--directed'], {b:0})]:
            with self.subTest(root=root, depth=depth, direction=direction, extra=extra):
                self.document['defaults']={'bfs':{'startUuid':root,'maxDepth':depth,'direction':direction}}
                self.save()
                output=self.root / 'reference-defaults.csv'
                result=subprocess.run([sys.executable,'-B',str(ROOT / 'tools/algorithms/bfs.py'),
                    '--graph-manifest',str(self.domain),'--output-csv',str(output),*extra],
                    cwd=self.root,capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                with output.open() as stream:
                    actual={row['vertex_id']:int(row['distance']) for row in csv.DictReader(stream,delimiter=';')}
                self.assertEqual(actual,expected)

    def test_cache_benchmark_missing_numeric_root_preserves_existing_output(self):
        self.document['defaults']={'bfs':{'startSource':{'namespace':'n','type':'Device','idType':'string','id':'7'}}}
        self.save()
        output=self.root / 'existing-cache-results';output.mkdir()
        sentinel=output / 'keep.txt';sentinel.write_text('previous measurements')
        plan=self.root / 'cache-plan.json';plan.write_text('{}')
        result=subprocess.run([sys.executable,'-B',str(ROOT / 'tools/measurement/benchmark_cache_configs.py'),
            '--domain',str(self.manifest),'--cache-plans',str(plan),'--output-dir',str(output),'--overwrite'],
            cwd=self.root,capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('polygraph.py run',result.stderr)
        self.assertEqual(sentinel.read_text(),'previous measurements')

    def test_suite_missing_numeric_roots_fails_before_service_lifecycle(self):
        import types
        from tools.measurement import benchmark_suite
        self.document['defaults']={'bfs':{'startSource':{'namespace':'n','type':'Device','idType':'string','id':'7'}}}
        self.save()
        suite=self.root / 'root-preflight.json'
        suite.write_text(json.dumps({'name':'root-preflight','pathBase':'config','startUuidCount':1,
            'datasets':[{'name':'inventory','domain':'arbitrary graph/domain.json'}],
            'outputDir':'results','build':{'enabled':False},'services':{'start':True,'resetVolumes':True}}))
        config=benchmark_suite.load_suite_config(suite)
        # Plotting is an unrelated optional dependency; the real graph resolver,
        # root validation and orchestration remain in use.
        plotting=types.ModuleType('tools.measurement.graph_bench.plotting')
        plotting.write_plots=lambda *args: []
        with patch.dict(sys.modules,{'tools.measurement.graph_bench.plotting':plotting}), \
             patch.object(benchmark_suite,'_start_services_for_project',side_effect=AssertionError('service lifecycle started')):
            with self.assertRaisesRegex(ValueError,'polygraph.py run'):
                benchmark_suite.run_suite(config,skip_build=True)


if __name__ == '__main__':
    unittest.main()
