from __future__ import annotations

import unittest
from collections import deque
from typing import Any

from tools.graphio.loaders import (
    BackendConnection,
    Neo4jHttpAdapter,
    Neo4jQueryError,
)


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


if __name__ == "__main__":
    unittest.main()
