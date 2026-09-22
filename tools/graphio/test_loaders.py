from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from collections import deque
from typing import Any
from unittest.mock import patch

from tools.graphio.id_codec import BackendLayout, canonical_global_id, to_signed_storage
from tools.graphio.loaders import (
    ArangoHttpAdapter,
    build_endpoint_directory,
    BackendConnection,
    EndpointInfo,
    Neo4jHttpAdapter,
    Neo4jQueryError,
)
from tools.graphio.model import NodeRecord, RelationshipRecord
from tools.graphio.writer import GraphJsonLinesWriter


def count_response(value: int) -> dict[str, Any]:
    return {"results": [{"data": [{"row": [value]}]}], "errors": []}


class RecordingNeo4jAdapter(Neo4jHttpAdapter):
    def __init__(self, outcomes: list[dict[str, Any] | BaseException]):
        super().__init__(
            BackendConnection(
                source="Neo4J",
                endpoint="http://localhost:7474/db/neo4j/tx/commit",
                database="neo4j",
                username="neo4j",
                password="password",
            )
        )
        self.outcomes = deque(outcomes)
        self.calls: list[tuple[str, dict[str, Any]]] = []

    def _cypher(
        self,
        statement: str,
        parameters: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        self.calls.append((statement, parameters or {}))
        outcome = self.outcomes.popleft()
        if isinstance(outcome, BaseException):
            raise outcome
        return outcome


class Neo4jResetTests(unittest.TestCase):
    def test_reset_deletes_relationships_before_nodes(self) -> None:
        adapter = RecordingNeo4jAdapter(
            [
                count_response(250),
                count_response(0),
                count_response(250),
                count_response(0),
            ]
        )

        adapter.reset(batch_size=250)

        self.assertEqual(len(adapter.calls), 4)
        self.assertTrue(all("DELETE r" in call[0] for call in adapter.calls[:2]))
        self.assertTrue(all("DELETE n" in call[0] for call in adapter.calls[2:]))
        self.assertTrue(all("DETACH DELETE" not in call[0] for call in adapter.calls))
        self.assertEqual([call[1]["limit"] for call in adapter.calls], [250, 250, 250, 250])

    def test_reset_reduces_batch_after_memory_pool_failure(self) -> None:
        memory_error = Neo4jQueryError(
            [
                {
                    "code": "Neo.TransientError.General.MemoryPoolOutOfMemoryError",
                    "message": 'Failed to commit transaction "bolt-test"',
                }
            ]
        )
        adapter = RecordingNeo4jAdapter(
            [
                memory_error,
                count_response(1),
                count_response(0),
                count_response(0),
            ]
        )

        adapter.reset(batch_size=250)

        self.assertEqual([call[1]["limit"] for call in adapter.calls], [250, 125, 125, 250])

    def test_reset_does_not_hide_non_memory_errors(self) -> None:
        original_error = Neo4jQueryError(
            [{"code": "Neo.ClientError.Statement.SyntaxError", "message": "bad query"}]
        )
        adapter = RecordingNeo4jAdapter([original_error])

        with self.assertRaises(Neo4jQueryError) as raised:
            adapter.reset(batch_size=250)

        self.assertIs(raised.exception, original_error)
        self.assertEqual(len(adapter.calls), 1)

    def test_reset_rejects_non_positive_batch_size(self) -> None:
        adapter = RecordingNeo4jAdapter([])

        with self.assertRaisesRegex(ValueError, "reset batch size must be positive"):
            adapter.reset(batch_size=0)


class Neo4jIntegerIdLoadTests(unittest.TestCase):
    def test_prepare_creates_local_id_and_proxy_constraints(self) -> None:
        adapter = RecordingNeo4jAdapter([
            {"results": [], "errors": []},
            {"results": [], "errors": []},
        ])

        adapter.prepare(["Person"], ["KNOWS"])

        self.assertEqual(len(adapter.calls), 2)
        self.assertIn("POLYGRAPHVertex", adapter.calls[0][0])
        self.assertIn("n.localId IS UNIQUE", adapter.calls[0][0])
        self.assertIn("Proxy", adapter.calls[1][0])
        self.assertIn("n.uuid IS UNIQUE", adapter.calls[1][0])

    def test_real_nodes_keep_stable_payload_local_ids(self) -> None:
        layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
        adapter = RecordingNeo4jAdapter([
            {
                "results": [{"data": [{"row": ["123", 0]}, {"row": ["124", 1]}]}],
                "errors": [],
            }
        ])
        mapping = adapter.insert_native_nodes(
            [
                NodeRecord(123, "Person", "Neo4J", {"uuid": 999, "name": "Alice"}),
                NodeRecord(124, "Person", "Neo4J", {"localId": 999, "name": "Bob"}),
            ],
            layout,
            first_local_id=0,
        )

        statement, parameters = adapter.calls[0]
        self.assertIn("n.localId = row.local_id", statement)
        self.assertNotIn("id(n)", statement)
        self.assertEqual([row["local_id"] for row in parameters["rows"]], [123, 124])
        self.assertNotIn("uuid", parameters["rows"][0]["props"])
        self.assertNotIn("localId", parameters["rows"][1]["props"])
        self.assertEqual(
            mapping,
            {123: 123, 124: 124},
        )

    def test_node_local_id_offset_does_not_rewrite_stable_payload(self) -> None:
        layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
        adapter = RecordingNeo4jAdapter([
            {"results": [{"data": [{"row": ["125", 2]}]}], "errors": []}
        ])

        mapping = adapter.insert_native_nodes(
            [NodeRecord(125, "Person", "Neo4J", {})],
            layout,
            first_local_id=2,
        )

        self.assertEqual(adapter.calls[0][1]["rows"][0]["local_id"], 125)
        self.assertEqual(mapping, {125: 125})

    def test_proxy_nodes_store_signed_uint64_uuid(self) -> None:
        adapter = RecordingNeo4jAdapter([{"results": [{"data": []}], "errors": []}])
        layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
        remote_id = layout.encode_local(0x1234_5678_9ABC, "ArangoDB", residency_mask=1 << 3)
        adapter.insert_proxy_nodes([
            NodeRecord(
                remote_id,
                "Proxy",
                "Neo4J",
                {"is_proxy": True, "remote-type": "Person", "uuid": 1},
            )
        ])
        statement, parameters = adapter.calls[0]
        self.assertIn("MERGE (n:Proxy", statement)
        self.assertEqual(parameters["rows"][0]["uuid"], to_signed_storage(canonical_global_id(remote_id)))
        self.assertNotIn("uuid", parameters["rows"][0]["props"])

    def test_native_relationship_has_no_uuid_and_uses_stable_payload_local_id(self) -> None:
        layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
        source = layout.encode_local(3, "Neo4J")
        target = layout.encode_local(4, "Neo4J")
        relationship = layout.encode_local(55, "Neo4J")
        adapter = RecordingNeo4jAdapter([
            {"results": [{"data": [{"row": [str(relationship), 9]}]}], "errors": []}
        ])
        mapping = adapter.insert_relationships(
            [RelationshipRecord(relationship, "KNOWS", "Neo4J", source, target, {"uuid": 8})],
            {source: "Person", target: "Person"},
            {
                source: EndpointInfo(source, "Person", "Neo4J"),
                target: EndpointInfo(target, "Person", "Neo4J"),
            },
            layout,
            first_local_id=9,
        )
        statement, parameters = adapter.calls[0]
        self.assertIn("MATCH (a:POLYGRAPHVertex {localId: row.from_id})", statement)
        self.assertNotIn("row.from_proxy", statement)
        self.assertNotIn(" OR ", statement)
        self.assertIn("SET r.localId = row.local_id", statement)
        self.assertIn("REMOVE r.uuid", statement)
        self.assertNotIn("RETURN", statement)
        self.assertEqual(parameters["rows"][0]["local_id"], 55)
        self.assertNotIn("uuid", parameters["rows"][0]["props"])
        self.assertEqual(mapping[relationship], relationship)

    def test_proxy_endpoint_uses_unique_proxy_uuid_seek_without_or_predicate(self) -> None:
        layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
        source = layout.encode_local(3, "ArangoDB")
        target = layout.encode_local(4, "Neo4J")
        relationship = layout.encode_local(55, "ArangoDB")
        adapter = RecordingNeo4jAdapter([
            {"results": [{"data": []}], "errors": []}
        ])
        adapter.insert_relationships(
            [RelationshipRecord(relationship, "KNOWS", "ArangoDB", source, target, {})],
            {source: "Proxy", target: "Person"},
            {
                source: EndpointInfo(source, "Person", "ArangoDB"),
                target: EndpointInfo(target, "Person", "Neo4J"),
            },
            layout,
        )
        statement, _ = adapter.calls[0]
        self.assertIn("MATCH (a:Proxy {uuid: row.from_id})", statement)
        self.assertIn("MATCH (b:POLYGRAPHVertex {localId: row.to_id})", statement)
        self.assertNotIn("row.from_proxy", statement)
        self.assertNotIn(" OR ", statement)


class ArangoBulkImportTests(unittest.TestCase):
    def test_document_import_uses_jsonl_and_persistent_session(self) -> None:
        connection = BackendConnection(
            source="ArangoDB",
            endpoint="http://localhost:8529/_db/university/_api/cursor",
            database="university",
            username="root",
            password="password",
        )
        adapter = ArangoHttpAdapter(connection, import_workers=1)
        documents = [{"_key": "1", "value": "a"}, {"_key": "2", "value": "b"}]

        with patch("tools.graphio.loaders._request_json", return_value={"errors": 0}) as request:
            adapter._import_batch("Person", documents)

        kwargs = request.call_args.kwargs
        self.assertIsNone(kwargs.get("payload"))
        self.assertEqual(
            kwargs["raw_body"],
            '{"_key":"1","value":"a"}\n{"_key":"2","value":"b"}\n',
        )
        self.assertIn("type=documents", request.call_args.args[1])
        self.assertIs(kwargs["session"], adapter._session())

    def test_large_single_collection_is_split_across_import_workers(self) -> None:
        connection = BackendConnection(
            "ArangoDB", "http://localhost:8529", "university", "root", "password"
        )
        adapter = ArangoHttpAdapter(connection, import_workers=4)
        documents = [{"_key": str(index)} for index in range(4_000)]

        with patch.object(adapter, "_import_batch") as import_batch:
            adapter._import_grouped({"Person": documents})

        self.assertEqual(import_batch.call_count, 4)
        self.assertEqual(
            sum(len(call.args[1]) for call in import_batch.call_args_list),
            len(documents),
        )
        self.assertTrue(all(call.args[0] == "Person" for call in import_batch.call_args_list))

    def test_rejects_non_positive_import_workers(self) -> None:
        connection = BackendConnection("ArangoDB", "http://localhost:8529", "university", "root", "password")
        with self.assertRaisesRegex(ValueError, "import_workers must be positive"):
            ArangoHttpAdapter(connection, import_workers=0)


class DiskBackedEndpointDirectoryTests(unittest.TestCase):
    def test_cache_bits_do_not_change_endpoint_lookup_identity(self) -> None:
        layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
        canonical = layout.encode_local(42, "Neo4J")
        decorated = layout.encode_local(42, "Neo4J", residency_mask=1 << 3)
        with tempfile.TemporaryDirectory() as temporary:
            graph = Path(temporary) / "graph.jsonl"
            with GraphJsonLinesWriter(graph, metadata=layout.metadata()) as writer:
                writer.write_node(NodeRecord(decorated, "Person", "Neo4J", {}))
            directory = build_endpoint_directory(graph, layout)
            try:
                self.assertEqual(directory[canonical].type, "Person")
                self.assertEqual(directory[decorated].global_id, canonical)
            finally:
                index_path = directory.path
                directory.close()
                index_path.unlink(missing_ok=True)



if __name__ == "__main__":
    unittest.main()
