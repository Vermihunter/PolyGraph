"""The existing cache engine consumes topology independently of dataset columns."""
import json
import struct
import tempfile
import unittest
from pathlib import Path

from tools.graphio.cache_planner import plan_graph_caches
from tools.graphio.id_codec import BackendLayout
from tools.graphio.reader import open_graph_reader


def check_unfamiliar_schema_cache_and_gpu_csr(tmp_path):
    graph = Path(__file__).parent / 'fixtures/schema_agnostic/graph.jsonl'
    original = graph.read_bytes()
    layout = BackendLayout(('Neo4J', 'ArangoDB', 'PostgreSQL'))
    config = {
        'version': 1,
        'gpus': [{'device_id': 0, 'memory': '64KiB', 'relation_limit': 100,
                  'regions': [{'name': 'all', 'policy': {'kind': 'top-degree', 'vertex_count': 4}}]}],
        'mmap': {'enabled': True, 'memory': '64KiB', 'vertex_limit': 4, 'policy': {'kind': 'best'}},
        'redis': {'enabled': True, 'memory': '64KiB', 'vertex_limit': 4, 'policy': {'kind': 'top-degree'}},
    }
    result = plan_graph_caches(graph, config, output_dir=tmp_path / 'cache', routing_layout=layout, planning_db_path=tmp_path / 'planning.sqlite3')
    plan_path = Path(result['plan'])
    plan = json.loads(plan_path.read_text())
    assert graph.read_bytes() == original
    reader = open_graph_reader(graph)
    expected = {}
    for edge in reader.iter_relationships():
        expected.setdefault(edge.from_uuid, set()).add(edge.to_uuid)
        expected.setdefault(edge.to_uuid, set()).add(edge.from_uuid)
    for kind in ('mmap', 'redis'):
        artifact = plan['artifacts'][kind]
        assert 0 < artifact['vertices'] <= 4
        root = plan_path.parent / kind
        index = (root / 'index.bin').read_bytes()
        neighbors = (root / 'neighbors.bin').read_bytes()
        actual = {}
        for node, offset, count in struct.iter_unpack('<QQQ', index):
            actual[node] = set(struct.unpack_from('<' + 'Q' * count, neighbors, offset * 8))
        assert actual == {node: expected[node] for node in actual}
    gpu_manifest = plan_path.parent / plan['artifacts']['gpu']['0']['manifest']
    assert gpu_manifest.is_file()
    manifest = json.loads(gpu_manifest.read_text())
    root = gpu_manifest.parent
    ids = [item[0] for item in struct.iter_unpack('<Q', (root / manifest['globalIds']).read_bytes())]
    sources = [item[0] for item in struct.iter_unpack('<I', (root / manifest['sources']).read_bytes())]
    targets = [item[0] for item in struct.iter_unpack('<I', (root / manifest['destinations']).read_bytes())]
    actual_pairs = {(ids[a], ids[b]) for a, b in zip(sources, targets, strict=True)}
    assert actual_pairs == {(node, neighbor) for node, neighbors in expected.items() for neighbor in neighbors}


class SchemaAgnosticCacheTest(unittest.TestCase):
    def test_cache_and_gpu_csr(self):
        with tempfile.TemporaryDirectory(prefix="polygraph-schema-cache-") as directory:
            check_unfamiliar_schema_cache_and_gpu_csr(Path(directory))
