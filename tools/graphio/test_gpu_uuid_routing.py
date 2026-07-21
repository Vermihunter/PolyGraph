from __future__ import annotations

import json
import tempfile
import unittest
import uuid
from collections import Counter
from pathlib import Path

from tools.graphio.domain import (
    generate_sample_graph,
    load_domain_manifest,
    materialize_graph_jsonl,
    record_native_id_resolution,
    write_backend_config_for_sources,
    write_domain_manifest,
)
from tools.graphio.id_codec import (
    BackendLayout,
    deterministic_routed_entity_uuid,
    parse_global_id,
    read_native_node_id_map,
    to_signed_storage,
    write_native_id_map,
)
from tools.graphio.reader import open_graph_reader


class GpuUuidRoutingTests(unittest.TestCase):
    def test_dynamic_bit_count(self) -> None:
        self.assertEqual(BackendLayout(("Neo4J",)).bit_count, 0)
        self.assertEqual(BackendLayout(("Neo4J", "ArangoDB")).bit_count, 1)
        self.assertEqual(BackendLayout(("Neo4J", "ArangoDB", "GPU")).bit_count, 2)
        self.assertEqual(BackendLayout(("Neo4J", "ArangoDB", "Memgraph", "GPU")).bit_count, 2)


    def test_metadata_declares_uint64_msb_layout(self) -> None:
        layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
        self.assertEqual(
            layout.metadata(),
            {
                "idRouting": ["Neo4J", "ArangoDB", "GPU"],
                "idRoutingBits": 2,
                "idRoutingPosition": "msb",
                "idType": "uint64",
            },
        )

    def test_signed_database_storage_round_trip_preserves_all_bits(self) -> None:
        value = 0xE123_4567_89AB_CDEF
        stored = to_signed_storage(value)
        self.assertLess(stored, 0)
        self.assertEqual(parse_global_id(stored), value)
        self.assertEqual(parse_global_id(str(stored)), value)

    def test_native_id_sidecar_streams_node_mapping(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "native-id-map.jsonl"
            write_native_id_map(
                path,
                node_mapping={10: 1000, 11: 1001},
                relationship_mapping={20: 2000},
            )
            self.assertEqual(read_native_node_id_map(path), {10: 1000, 11: 1001})
            records = [json.loads(line) for line in path.read_text().splitlines()]
        self.assertEqual(records[0]["format"], "cpgdb.native-id-map")
        self.assertEqual(records[-1]["record"], "relationship")

    def test_three_backend_codes_are_00_01_10(self) -> None:
        layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
        base = 0x38_9D_56_CE_57_3B_F1_9C
        neo = layout.encode(base, "Neo4J")
        arango = layout.encode(base, "ArangoDB")
        gpu = layout.encode(base, "GPU")
        self.assertEqual(layout.decode_code(neo), 0)
        self.assertEqual(layout.decode_code(arango), 1)
        self.assertEqual(layout.decode_code(gpu), 2)
        self.assertEqual(layout.local_id(neo), layout.local_id(arango))
        self.assertEqual(layout.local_id(arango), layout.local_id(gpu))
        self.assertEqual(neo >> layout.payload_bits, 0)
        self.assertEqual(arango >> layout.payload_bits, 1)
        self.assertEqual(gpu >> layout.payload_bits, 2)

    def test_three_backend_generator_never_emits_unused_code(self) -> None:
        layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
        counts: Counter[str] = Counter()
        for index in range(500):
            value, source = deterministic_routed_entity_uuid(
                "node", "Person", index, layout
            )
            self.assertEqual(layout.decode_source(value), source)
            self.assertLess(layout.decode_code(value), 3)
            counts[source] += 1
        self.assertEqual(set(counts), {"Neo4J", "ArangoDB", "GPU"})


    def test_domain_manifest_tracks_source_resolved_graph_and_start_mapping(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("schema.json", "graph.jsonl", "backends.json", "resolved.jsonl"):
                (root / name).write_text("{}\n", encoding="utf-8")
            manifest = write_domain_manifest(
                root / "domain.json",
                domain_id="test",
                name="test",
                description="",
                schema=root / "schema.json",
                data=root / "graph.jsonl",
                backends=root / "backends.json",
                start_uuid="10",
            )
            mapping_path = root / "native-id-map.jsonl"
            write_native_id_map(mapping_path, node_mapping={10: 100}, relationship_mapping={})
            resolved = record_native_id_resolution(
                load_domain_manifest(manifest),
                resolved_graph=root / "resolved.jsonl",
                native_id_map=mapping_path,
            )
            raw = json.loads(manifest.read_text(encoding="utf-8"))

        self.assertEqual(resolved.start_uuid, "100")
        self.assertEqual(resolved.paths.data.name, "resolved.jsonl")
        self.assertEqual(resolved.paths.source_data.name, "graph.jsonl")
        self.assertEqual(resolved.paths.native_id_map.name, "native-id-map.jsonl")
        self.assertEqual(raw["defaults"]["bfs"]["sourceStartUuid"], "10")

    def test_schema_type_placement_is_default(self) -> None:
        schema = {
            "domain": "schema-placement-test",
            "nodeTypes": [
                {"name": "University", "source": "Neo4J", "properties": []},
                {"name": "Student", "source": "ArangoDB", "properties": []},
                {"name": "Course", "source": "GPU", "properties": []},
            ],
            "relations": [
                {
                    "name": "ENROLLED_IN",
                    "from": "Student",
                    "to": "Course",
                    "source": "GPU",
                    "directed": True,
                    "properties": [],
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            schema_path = root / "schema.json"
            graph_path = root / "graph.jsonl"
            schema_path.write_text(json.dumps(schema), encoding="utf-8")

            generate_sample_graph(schema_path, graph_path, nodes_per_type=12)

            reader = open_graph_reader(graph_path)
            metadata = reader.metadata()
            self.assertEqual(metadata["idRoutingStrategy"], "schema-type-placement-v1")
            self.assertEqual(metadata["idRouting"], ["Neo4J", "ArangoDB", "GPU"])
            self.assertEqual(metadata["idRoutingBits"], 2)

            expected_node_sources = {
                "University": "Neo4J",
                "Student": "ArangoDB",
                "Course": "GPU",
            }
            layout = BackendLayout(tuple(metadata["idRouting"]))
            for node in reader.iter_nodes():
                self.assertEqual(node.source, expected_node_sources[node.type])
                self.assertEqual(layout.decode_source(node.uuid), node.source)
            for relationship in reader.iter_relationships():
                self.assertEqual(relationship.source, "GPU")
                self.assertEqual(layout.decode_source(relationship.uuid), "GPU")


    def test_source_free_schema_generates_gpu_routed_jsonl(self) -> None:
        schema = {
            "domain": "routing-test",
            "nodeTypes": [
                {"name": "Person", "properties": []},
                {"name": "Post", "properties": []},
            ],
            "relations": [
                {
                    "name": "WROTE",
                    "from": "Person",
                    "to": "Post",
                    "directed": True,
                    "properties": [],
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            schema_path = root / "schema.json"
            graph_path = root / "graph.jsonl"
            backend_path = root / "backends.json"
            schema_path.write_text(json.dumps(schema), encoding="utf-8")

            generate_sample_graph(
                schema_path,
                graph_path,
                nodes_per_type=30,
                placement_strategy="identity-hash",
                routing_backends=("Neo4J", "ArangoDB", "GPU"),
            )
            write_backend_config_for_sources(
                ("Neo4J", "ArangoDB", "GPU"),
                backend_path,
                arango_database="routing_test",
            )

            reader = open_graph_reader(graph_path)
            metadata = reader.metadata()
            self.assertEqual(metadata["idRoutingBits"], 2)
            self.assertEqual(metadata["idRouting"], ["Neo4J", "ArangoDB", "GPU"])
            self.assertEqual(metadata["idRoutingStrategy"], "identity-hash-v1")

            layout = BackendLayout(tuple(metadata["idRouting"]))
            node_sources = {layout.decode_source(node.uuid) for node in reader.iter_nodes()}
            self.assertEqual(node_sources, {"Neo4J", "ArangoDB", "GPU"})

            backend_config = json.loads(backend_path.read_text(encoding="utf-8"))
            self.assertEqual(backend_config["idRouting"], metadata["idRouting"])
            enabled = {
                entry["id"]
                for entry in backend_config["backends"]
                if entry.get("enabled", True)
            }
            self.assertEqual(enabled, {"Neo4J", "ArangoDB", "GPU"})

    def test_legacy_input_is_migrated_with_schema_placement(self) -> None:
        schema = {
            "domain": "legacy-migration-test",
            "idRouting": ["Neo4J", "ArangoDB", "GPU"],
            "nodeTypes": [
                {"name": "University", "source": "Neo4J", "properties": []},
                {"name": "Student", "source": "ArangoDB", "properties": []},
            ],
            "relations": [
                {
                    "name": "STUDIES_AT",
                    "from": "Student",
                    "to": "University",
                    "source": "GPU",
                    "properties": [],
                }
            ],
        }
        legacy = {
            "domain": "legacy",
            "nodes": [
                {
                    "uuid": "00000000-0000-4000-8000-000000000001",
                    "type": "University",
                    "source": "ArangoDB",
                    "properties": {"name": "U"},
                },
                {
                    "uuid": "00000000-0000-4000-8000-000000000002",
                    "type": "Student",
                    "source": "Neo4J",
                    "properties": {"name": "S"},
                },
            ],
            "relationships": [
                {
                    "uuid": "00000000-0000-4000-8000-000000000003",
                    "type": "STUDIES_AT",
                    "source": "Neo4J",
                    "from": "00000000-0000-4000-8000-000000000002",
                    "to": "00000000-0000-4000-8000-000000000001",
                    "properties": {},
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            schema_path = root / "schema.json"
            input_path = root / "legacy.json"
            output_path = root / "graph.jsonl"
            schema_path.write_text(json.dumps(schema), encoding="utf-8")
            input_path.write_text(json.dumps(legacy), encoding="utf-8")

            materialize_graph_jsonl(
                input_path,
                output_path,
                schema_path=schema_path,
                migrate_missing_routing=True,
            )

            reader = open_graph_reader(output_path)
            metadata = reader.metadata()
            self.assertEqual(metadata["idRoutingStrategy"], "schema-type-placement-v1")
            layout = BackendLayout(tuple(metadata["idRouting"]))
            nodes = list(reader.iter_nodes())
            self.assertEqual(
                {node.type: node.source for node in nodes},
                {"University": "Neo4J", "Student": "ArangoDB"},
            )
            self.assertTrue(all(layout.decode_source(node.uuid) == node.source for node in nodes))
            node_ids = {node.uuid for node in nodes}
            relationship = next(reader.iter_relationships())
            self.assertEqual(relationship.source, "GPU")
            self.assertEqual(layout.decode_source(relationship.uuid), "GPU")
            self.assertIn(relationship.from_uuid, node_ids)
            self.assertIn(relationship.to_uuid, node_ids)
            self.assertFalse((root / ".graph.jsonl.uuid-map.sqlite3").exists())

    def test_legacy_input_can_be_migrated_with_identity_hash(self) -> None:
        schema = {
            "domain": "legacy-hash-test",
            "idRouting": ["Neo4J", "ArangoDB", "GPU"],
            "nodeTypes": [
                {"name": "University", "source": "Neo4J", "properties": []}
            ],
            "relations": [],
        }
        legacy = {
            "nodes": [
                {
                    "uuid": str(uuid.UUID(int=index + 1)),
                    "type": "University",
                    "source": "Neo4J",
                    "properties": {"index": index},
                }
                for index in range(120)
            ],
            "relationships": [],
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            schema_path = root / "schema.json"
            input_path = root / "legacy.json"
            output_path = root / "graph.jsonl"
            schema_path.write_text(json.dumps(schema), encoding="utf-8")
            input_path.write_text(json.dumps(legacy), encoding="utf-8")

            materialize_graph_jsonl(
                input_path,
                output_path,
                schema_path=schema_path,
                placement_strategy="identity-hash",
                migrate_missing_routing=True,
            )

            reader = open_graph_reader(output_path)
            metadata = reader.metadata()
            self.assertEqual(metadata["idRoutingStrategy"], "identity-hash-v1")
            layout = BackendLayout(tuple(metadata["idRouting"]))
            nodes = list(reader.iter_nodes())
            self.assertEqual({node.source for node in nodes}, {"Neo4J", "ArangoDB", "GPU"})
            self.assertTrue(all(layout.decode_source(node.uuid) == node.source for node in nodes))


if __name__ == "__main__":
    unittest.main()
