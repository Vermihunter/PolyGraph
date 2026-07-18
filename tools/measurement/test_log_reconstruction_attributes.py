from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.event_log_average import build_average_timeline, load_run, write_average_jsonl
from tools.measurement.log_reconstruction import (
    build_attributes_html,
    load_jsonl,
    make_hover_text,
    pair_spans,
)


class LogReconstructionAttributeTests(unittest.TestCase):
    def _write_log(self, path: Path, *, query_rows: int = 42) -> None:
        events = [
            {
                "event": "neo4j.expand.start",
                "seq": 1,
                "steady_ns": 1_000,
                "thread_id": 7,
                "backend": "Neo4J",
                "depth": 3,
                "fields": {
                    "query_rows": query_rows,
                    "cache": {"hits": 9},
                },
                "labels": ["Person", "Post"],
            },
            {
                "event": "neo4j.expand.end",
                "seq": 2,
                "steady_ns": 6_000,
                "thread_id": 7,
                "result_count": 123,
                "duration_us": 5.0,
            },
        ]
        path.write_text(
            "".join(json.dumps(event) + "\n" for event in events),
            encoding="utf-8",
        )

    def test_all_custom_fields_are_preserved_and_visible(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "run.jsonl"
            self._write_log(path)
            events = load_jsonl(path)
            spans = pair_spans(events)

        self.assertEqual(len(spans), 1)
        row = spans.iloc[0]
        self.assertEqual(row["attr.backend"], "Neo4J")
        self.assertEqual(row["attr.query_rows"], 42)
        self.assertEqual(row["attr.cache.hits"], 9)
        self.assertEqual(row["attr.result_count"], 123)
        self.assertEqual(row["attr.labels"], '["Person","Post"]')

        hover = make_hover_text(row)
        self.assertIn("backend: Neo4J", hover)
        self.assertIn("query_rows: 42", hover)
        self.assertIn("cache.hits: 9", hover)
        self.assertIn("result_count: 123", hover)

        run = {"safe_id": "run", "spans": spans}
        attributes_html = build_attributes_html(run)
        self.assertIn("query_rows", attributes_html)
        self.assertIn("result_count", attributes_html)

    def test_missing_dataframe_fields_do_not_become_nan_attributes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "run.jsonl"
            self._write_log(path)
            spans = pair_spans(load_jsonl(path))

        attributes = spans.iloc[0]["attributes"]
        self.assertNotIn("start.result_count", attributes)
        self.assertNotIn("end.backend", attributes)
        self.assertNotIn("start.query_rows", attributes)
        self.assertEqual(attributes["result_count"], 123)

    def test_average_execution_keeps_stable_and_numeric_attributes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "first.jsonl"
            second = root / "second.jsonl"
            output = root / "average.jsonl"
            self._write_log(first, query_rows=40)
            self._write_log(second, query_rows=44)

            timeline = build_average_timeline([load_run(first), load_run(second)])
            write_average_jsonl(output, timeline, run_count=2)
            generated = [json.loads(line) for line in output.read_text().splitlines()]

        start = next(event for event in generated if event["event"].endswith(".start"))
        self.assertEqual(start["backend"], "Neo4J")
        self.assertEqual(start["query_rows"], 42.0)
        self.assertEqual(start["cache.hits"], 9)
        self.assertEqual(start["labels"], '["Person","Post"]')


if __name__ == "__main__":
    unittest.main()
