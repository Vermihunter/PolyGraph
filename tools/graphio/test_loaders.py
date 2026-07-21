from __future__ import annotations

import unittest
from collections import deque
from typing import Any

from tools.graphio.id_codec import BackendLayout, to_signed_storage
from tools.graphio.loaders import (
    BackendConnection,
    EndpointInfo,
    Neo4jHttpAdapter,
    Neo4jQueryError,
)
from tools.graphio.model import NodeRecord, RelationshipRecord


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


class Neo4jNativeIdLoadTests(unittest.TestCase):
    def test_real_nodes_use_native_ids_without_uuid_property(self) -> None:
        layout = BackendLayout(("Neo4J", "ArangoDB", "GPU"))
        adapter = RecordingNeo4jAdapter([
            {"results": [{"data": [{"row": ["123", 7]}]}], "errors": []}
        ])
        mapping = adapter.insert_native_nodes(
            [NodeRecord(123, "Person", "Neo4J", {"uuid": 999, "name": "Alice"})],
            layout,
        )

        statement, parameters = adapter.calls[0]
        self.assertIn("id(n) AS native_id", statement)
        self.assertNotIn("n.uuid", statement)
        self.assertNotIn("uuid", parameters["rows"][0]["props"])
        self.assertEqual(mapping, {123: layout.encode_local(7, "Neo4J")})

    def test_proxy_nodes_store_signed_uint64_uuid(self) -> None:
        adapter = RecordingNeo4jAdapter([{"results": [{"data": []}], "errors": []}])
        remote_id = 0xE123_4567_89AB_CDEF
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
        self.assertEqual(parameters["rows"][0]["uuid"], to_signed_storage(remote_id))
        self.assertNotIn("uuid", parameters["rows"][0]["props"])

    def test_native_relationship_has_no_uuid_and_returns_id_r(self) -> None:
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
        )
        statement, parameters = adapter.calls[0]
        self.assertIn("id(a) = row.from_id", statement)
        self.assertNotIn("row.from_proxy", statement)
        self.assertNotIn(" OR ", statement)
        self.assertIn("id(r) AS native_id", statement)
        self.assertIn("REMOVE r.uuid", statement)
        self.assertNotIn("uuid", parameters["rows"][0]["props"])
        self.assertEqual(mapping[relationship], layout.encode_local(9, "Neo4J"))

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
        self.assertIn("MATCH (b) WHERE id(b) = row.to_id", statement)
        self.assertNotIn("row.from_proxy", statement)
        self.assertNotIn(" OR ", statement)


if __name__ == "__main__":
    unittest.main()
