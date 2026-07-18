from __future__ import annotations

import csv
import json
import tempfile
import unittest
from pathlib import Path

from tools.measurement.benchmark_merge import DatasetInput, merge_benchmark_results
from tools.measurement.graph_bench.metrics import MeasurementSink, RunRecord


class BenchmarkMergeTests(unittest.TestCase):
    def _write_source(self, root: Path, records: list[RunRecord], artifact_name: str) -> Path:
        root.mkdir(parents=True, exist_ok=True)
        sink = MeasurementSink(root)
        for record in records:
            sink.add(record)
        sink.flush_all()
        output = root / "outputs" / artifact_name
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text("uuid,depth\na,0\n", encoding="utf-8")
        return root

    def test_prefer_ok_replaces_failed_retry_and_rebases_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            first = base / "first" / "datasets" / "schema-old"
            second = base / "second" / "datasets" / "schema-resume"
            destination = base / "merged"

            failed_output = first / "outputs" / "native.csv"
            ok_output = second / "outputs" / "native.csv"
            self._write_source(
                first,
                [
                    RunRecord(
                        timestamp="2026-01-01T00:00:00Z",
                        scenario="native_full",
                        system="neo4j_native",
                        phase="load",
                        repetition=1,
                        start_uuid="",
                        seconds=10.0,
                        status="failed",
                        details={"source": str(first / "generated" / "graph.jsonl")},
                    ),
                    RunRecord(
                        timestamp="2026-01-01T00:00:01Z",
                        scenario="distributed",
                        system="graph_db_distributed",
                        phase="bfs_query",
                        repetition=1,
                        start_uuid="root-a",
                        seconds=2.0,
                        status="ok",
                        output_csv=str(first / "outputs" / "distributed.csv"),
                        same_as_reference=True,
                        vertex_count=7,
                    ),
                ],
                "first.csv",
            )
            self._write_source(
                second,
                [
                    RunRecord(
                        timestamp="2026-01-02T00:00:00Z",
                        scenario="native_full",
                        system="neo4j_native",
                        phase="load",
                        repetition=1,
                        start_uuid="",
                        seconds=5.0,
                        status="ok",
                        details={"source": str(second / "generated" / "graph.jsonl")},
                    ),
                    RunRecord(
                        timestamp="2026-01-02T00:00:01Z",
                        scenario="native_full",
                        system="neo4j_native",
                        phase="bfs_query",
                        repetition=1,
                        start_uuid="root-a",
                        seconds=1.0,
                        status="ok",
                        output_csv=str(ok_output),
                        same_as_reference=True,
                        vertex_count=7,
                    ),
                ],
                "second.csv",
            )

            merge_benchmark_results(
                [
                    DatasetInput("schema-based", first),
                    DatasetInput("schema-based", second),
                ],
                output_dir=destination,
                run_id="merged-run",
                conflict_policy="prefer-ok",
            )

            merged_dataset = destination / "datasets" / "schema-based"
            rows = [
                json.loads(line)
                for line in (merged_dataset / "measurements.jsonl").read_text(encoding="utf-8").splitlines()
                if line.strip()
            ]
            load_rows = [
                row
                for row in rows
                if row["scenario"] == "native_full"
                and row["system"] == "neo4j_native"
                and row["phase"] == "load"
            ]
            self.assertEqual(len(load_rows), 1)
            self.assertEqual(load_rows[0]["status"], "ok")
            self.assertEqual(load_rows[0]["seconds"], 5.0)
            self.assertTrue(load_rows[0]["details"]["source"].startswith(str(merged_dataset)))
            self.assertTrue((merged_dataset / "outputs" / "first.csv").is_file())
            self.assertTrue((merged_dataset / "outputs" / "second.csv").is_file())

            summary = json.loads((destination / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "ok")
            self.assertEqual(summary["failedRecords"], 0)
            self.assertEqual(summary["datasets"][0]["mergedRecordCount"], 3)

            with (destination / "measurements.csv").open("r", encoding="utf-8", newline="") as handle:
                aggregate = list(csv.DictReader(handle))
            self.assertEqual({row["dataset"] for row in aggregate}, {"schema-based"})


if __name__ == "__main__":
    unittest.main()
