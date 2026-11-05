"""Preparation overhead comparisons must preserve graph semantics and timer units."""
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

from tools.ingestion.benchmark_partitioning import graph_fingerprint, summarize


class PartitionBenchmarkTest(unittest.TestCase):
    def test_fingerprint_ignores_only_generated_partition_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'graph.sqlite3'
            with sqlite3.connect(path) as db:
                db.executescript('CREATE TABLE nodes(id,original,key,type,owner,residency,properties);'
                                 'CREATE TABLE edges(id,original,type,owner,from_id,to_id,properties);'
                                 'CREATE TABLE proxies(backend,node_id); CREATE TABLE replicas(backend,edge_id);')
                db.execute('INSERT INTO nodes VALUES(1,1,?,?,?,?,?)', ('Person:1', 'Person', 'Neo4J', 0, '{"name":"Ž😀"}'))
                db.execute('INSERT INTO edges VALUES(2,2,?,?,?,?,?)', ('LINK', 'Neo4J', 1, 1, '{"value":7,"partition_id":9}'))
                db.execute('INSERT INTO proxies VALUES(?,?)', ('ArangoDB', 1))
                db.execute('INSERT INTO replicas VALUES(?,?)', ('Neo4J', 2))
            baseline = graph_fingerprint(path)
            with sqlite3.connect(path) as db:
                db.execute('UPDATE nodes SET properties=?', (json.dumps({'name': 'Ž😀', 'partition_id': 0}),))
                db.execute('UPDATE edges SET properties=?', (json.dumps({'value': 7, 'partition_id': 9, 'from_partition_id': 0,
                    'to_partition_id': 0, 'is_partition_cut': False}),))
            self.assertEqual(graph_fingerprint(path), baseline)
            with sqlite3.connect(path) as db:
                db.execute('UPDATE edges SET properties=?', ('{"value":7,"partition_id":10}',))
            self.assertNotEqual(graph_fingerprint(path), baseline)
            with sqlite3.connect(path) as db:
                db.execute('UPDATE edges SET properties=?', ('{"value":7,"partition_id":9}',))
            with sqlite3.connect(path) as db:
                db.execute('UPDATE edges SET to_id=999')
            self.assertNotEqual(graph_fingerprint(path), baseline)
            with sqlite3.connect(path) as db:
                db.execute('UPDATE edges SET to_id=1')
                db.execute('UPDATE nodes SET properties=?', ('{"name":"changed","partition_id":0}',))
            self.assertNotEqual(graph_fingerprint(path), baseline)

    def test_summary_reports_overhead_without_mixing_verification_time(self):
        trials = [{'variant': 'none', 'client': {'wall_seconds': 10}, 'verification_seconds': 200},
                  {'variant': 'k2', 'client': {'wall_seconds': 14}, 'verification_seconds': 300},
                  {'variant': 'none', 'client': {'wall_seconds': 12}, 'verification_seconds': 400},
                  {'variant': 'k2', 'client': {'wall_seconds': 16}, 'verification_seconds': 500}]
        summary = summarize(trials)
        self.assertEqual(summary['none']['mean_prepare_wall_seconds'], 11)
        self.assertEqual(summary['k2']['mean_prepare_wall_seconds'], 15)
        self.assertEqual(summary['k2']['overhead_seconds'], 4)
        self.assertAlmostEqual(summary['k2']['overhead_percent'], 100 * 4 / 11)
        self.assertEqual(summary['k2']['min_prepare_wall_seconds'], 14)
        self.assertEqual(summary['k2']['max_prepare_wall_seconds'], 16)


if __name__ == '__main__':
    unittest.main()
