"""Compare native artifacts to the existing Python converter/loader, without services."""
from __future__ import annotations

import csv
from dataclasses import replace
import json
import os
from pathlib import Path
import sqlite3
import shutil
import subprocess
import tempfile
import unittest

from tools.graphio.id_codec import BackendLayout, canonical_global_id, local_payload, to_signed_storage
from tools.graphio.loaders import EndpointInfo, _build_proxy_node, relationship_stored_on
from tools.graphio.domain import sanitize_name
from tools.ldbc_snb.convert import discover_csv_files, iter_nodes, iter_relationships, load_placement

FIXTURE = Path(__file__).resolve().parents[2] / 'tests/fixtures/ingestion'


class NativeSemanticsTest(unittest.TestCase):
    def setUp(self):
        self.binary = os.environ.get('POLYGRAPH_BULK_IMPORT')
        if not self.binary:
            self.skipTest('POLYGRAPH_BULK_IMPORT is required; CTest supplies the built executable')
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.placement = load_placement(str(FIXTURE / 'placement.json'), 'Neo4J', ('Neo4J', 'ArangoDB', 'PostgreSQL'))
        grouped = discover_csv_files(FIXTURE / 'raw')
        self.nodes = list(iter_nodes(grouped, self.placement))
        self.edges = list(iter_relationships(grouped, self.placement))
        self.layout = BackendLayout(('Neo4J', 'ArangoDB', 'PostgreSQL'))
        self.index = {n.uuid: EndpointInfo(n.uuid, n.type, n.source) for n in self.nodes}

    def prepare(self, adapter, input_path, output):
        subprocess.run([self.binary, 'prepare', '--adapter', adapter, '--input', str(input_path),
                        '--placement-config', str(FIXTURE / 'placement.json'), '--output-dir', str(output),
                        '--memory-limit-mb', '32', '--max-record-mb', '4', '--chunk-size-mb', '1'], check=True, capture_output=True)
        return json.loads((output / 'manifest.json').read_text())

    def selected(self, backend):
        edges = [e for e in self.edges if relationship_stored_on(e, backend, self.index, self.layout)]
        proxies = sorted({v for e in edges for v in (e.from_uuid, e.to_uuid) if self.layout.decode_source(v) != backend})
        nodes = [n for n in self.nodes if n.source == backend]
        nodes += [_build_proxy_node(backend, v, self.index[v]) for v in proxies]
        # load_postgresql_source preserves logical types for SQL traversal routing.
        if backend == 'PostgreSQL':
            nodes = [replace(n, type=self.index[n.uuid].type) if n.properties.get('is_proxy') else n for n in nodes]
        return nodes, edges

    def test_python_golden_and_both_adapters_match_native_representations(self):
        golden = json.loads((FIXTURE / 'python-golden.json').read_text())
        self.assertEqual(golden, {'nodes': [n.to_dict() for n in self.nodes], 'edges': [e.to_dict() for e in self.edges]})
        manifests = []
        for adapter, input_path in [('ldbc-snb', FIXTURE / 'raw'), ('polygraph-jsonl', FIXTURE / 'canonical.jsonl')]:
            output = self.root / adapter
            m = self.prepare(adapter, input_path, output)
            manifests.append(m)
            with sqlite3.connect(output / m['index']['path']) as db:
                actual = {i: (t, s, json.loads(p)) for i, t, s, p in db.execute('SELECT id,type,owner,properties FROM nodes')}
            self.assertEqual(actual, {to_signed_storage(n.uuid): (n.type, n.source, n.properties) for n in self.nodes})
            self.check_postgres(output, m)
            self.check_arango(output, m)
            self.check_neo4j(output, m)
            subprocess.run([self.binary, 'verify', '--manifest', str(output / 'manifest.json'), '--strict'], check=True, capture_output=True)
        for backend in self.layout.backends:
            a, b = (m['backends'][backend] for m in manifests)
            self.assertEqual([x['sha256'] for x in a['artifacts']], [x['sha256'] for x in b['artifacts']])

    def check_postgres(self, output, m):
        nodes, edges = self.selected('PostgreSQL')
        actual_nodes, actual_edges = {}, {}
        for a in m['backends']['PostgreSQL']['artifacts']:
            with (output / a['path']).open(newline='') as f:
                for row in csv.reader(f):
                    if a['kind'] == 'nodes':
                        actual_nodes[int(row[0])] = (row[1:5], json.loads(row[5]))
                    else:
                        actual_edges[int(row[0])] = (row[1:7], json.loads(row[7]))
        expected_nodes = {}
        for n in nodes:
            p = dict(n.properties)
            proxy = p.pop('is_proxy', False)
            expected_nodes[to_signed_storage(n.uuid)] = ([n.type, 'PostgreSQL', str(proxy).lower(), '0'], p)
        self.assertEqual(actual_nodes, expected_nodes)
        expected_edges = {}
        for e in edges:
            types = [self.index[v].type for v in (e.from_uuid, e.to_uuid)]
            expected_edges[to_signed_storage(e.uuid)] = ([e.type, e.source, str(to_signed_storage(e.from_uuid)), types[0],
                                                       str(to_signed_storage(e.to_uuid)), types[1]], e.properties)
        self.assertEqual(actual_edges, expected_edges)

    def check_arango(self, output, m):
        nodes, edges = self.selected('ArangoDB')
        actual = {}
        for a in m['backends']['ArangoDB']['artifacts']:
            for line in (output / a['path']).read_text().splitlines():
                doc = json.loads(line)
                actual[(a['collection'], doc['_key'])] = doc
        expected = {}
        types = {n.uuid: n.type for n in nodes}
        for n in nodes:
            doc = {**n.properties, '_key': str(n.uuid), 'uuid': to_signed_storage(n.uuid), 'type': n.type, 'source': n.source}
            expected[(sanitize_name(n.type), str(n.uuid))] = doc
        for e in edges:
            doc = {**e.properties, '_key': str(e.uuid), 'uuid': to_signed_storage(e.uuid), 'type': e.type, 'source': e.source,
                   '_from': f'{sanitize_name(types[e.from_uuid])}/{e.from_uuid}', '_to': f'{sanitize_name(types[e.to_uuid])}/{e.to_uuid}'}
            expected[(sanitize_name(e.type), str(e.uuid))] = doc
        self.assertEqual(actual, expected)

    def check_neo4j(self, output, m):
        nodes, edges = self.selected('Neo4J')
        actual_nodes, actual_edges = {}, {}
        for a in m['backends']['Neo4J']['artifacts']:
            with (output / a['path']).open(newline='') as f:
                for row in csv.DictReader(f):
                    if a['kind'] == 'edges':
                        key = (int(row['localId:long']) | (1 << 48)) if row['localId:long'] else int(row['uuid:long']) & ((1 << 64) - 1)
                        actual_edges[key] = row
                    else:
                        actual_nodes[int(row[':ID'])] = row
        self.assertEqual(set(actual_nodes), {n.uuid for n in nodes})
        self.assertEqual(set(actual_edges), {e.uuid for e in edges})
        for n in nodes:
            row = actual_nodes[n.uuid]
            labels = set(row[':LABEL'].split('\x1f'))
            self.assertTrue({'POLYGRAPHVertex', '__GraphNode', sanitize_name(n.type)} <= labels)
            if n.type == 'Proxy':
                self.assertEqual(int(row['uuid:long']), to_signed_storage(n.uuid))
                self.assertNotIn('localId:long', row)
            else:
                self.assertEqual(int(row['localId:long']), local_payload(n.uuid))
                self.assertNotIn('uuid:long', row)
                self.assertEqual(row['firstName:string'], n.properties['firstName'])
                self.assertEqual(row['email:string[]'].split('\x1f'), n.properties['email'])
        for e in edges:
            row = actual_edges[e.uuid]
            self.assertEqual((int(row[':START_ID']), int(row[':END_ID'])), (e.from_uuid, e.to_uuid))
            self.assertEqual(row['source:string'], e.source)
            self.assertEqual(row[':TYPE'], sanitize_name(e.type))

    def test_sparse_high_ldbc_prefix_ids_and_relationship_sequences_match_python(self):
        raw = self.root / 'raw'
        shutil.copytree(FIXTURE / 'raw', raw)
        high = (1 << 60) + 7
        (raw / 'dynamic/person_0_1.csv').write_text(f'id|firstName\n{high}|李\n')
        (raw / 'dynamic/person_likes_post_0_1.csv').write_text(f'from|to\n{high}|2\n')
        placement = load_placement(str(FIXTURE / 'placement.json'), 'Neo4J', self.layout.backends)
        grouped = discover_csv_files(raw)
        nodes = list(iter_nodes(grouped, placement))
        edges = list(iter_relationships(grouped, placement))
        output = self.root / 'high-artifacts'
        m = self.prepare('ldbc-snb', raw, output)
        with sqlite3.connect(output / m['index']['path']) as db:
            actual_nodes = {i: (key, json.loads(props)) for i, key, props in db.execute('SELECT id,key,properties FROM nodes')}
            actual_edges = {i: (f, t) for i, f, t in db.execute('SELECT id,from_id,to_id FROM edges')}
        self.assertEqual(actual_nodes, {to_signed_storage(n.uuid): (f'{n.type}:{n.properties["ldbc_id"]}', n.properties) for n in nodes})
        self.assertEqual(actual_edges, {to_signed_storage(e.uuid): (to_signed_storage(e.from_uuid), to_signed_storage(e.to_uuid)) for e in edges})

    def test_cli_rejects_unknown_options_and_does_not_execute_without_confirmation(self):
        self.assertEqual(subprocess.run([self.binary, 'prepare', '--unknown'], capture_output=True).returncode, 2)
        self.assertNotEqual(subprocess.run([self.binary, 'execute', '--manifest', 'missing', '--target-config', 'missing'], capture_output=True).returncode, 0)
        help_text = subprocess.check_output([self.binary, '--help'], text=True)
        for option in ('--strict', '--confirm-create', '--partition-map', '--chunk-size-mb', '--max-record-mb'):
            self.assertIn(option, help_text)


if __name__ == '__main__':
    unittest.main()
