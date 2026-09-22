from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.event_log_average import build_average_timeline, load_run, write_average_jsonl
from tools.measurement.log_reconstruction import (
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

    def test_out_of_order_seq_is_reordered_without_loading_all_events(self) -> None:
        events = [
            {"event": "query.start", "seq": 1, "steady_ns": 100, "thread_id": 7},
            {"event": "query.start", "seq": 3, "steady_ns": 300, "thread_id": 7},
            {"event": "query.end", "seq": 2, "steady_ns": 200, "thread_id": 7},
            {"event": "query.end", "seq": 4, "steady_ns": 400, "thread_id": 7},
        ]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "out-of-order.jsonl"
            path.write_text(
                "".join(json.dumps(event) + "\n" for event in events),
                encoding="utf-8",
            )
            events_frame = load_jsonl(path)
            spans = pair_spans(events_frame)
            averaged_run = load_run(path)

        self.assertEqual(len(events_frame), 4)
        self.assertEqual(len(spans), 2)
        self.assertEqual(spans["seq_start"].tolist(), [1, 3])
        self.assertEqual(spans["seq_end"].tolist(), [2, 4])
        self.assertEqual(len(averaged_run.spans), 2)
        self.assertEqual(
            [span.duration_ms for span in averaged_run.spans],
            [0.0001, 0.0001],
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
