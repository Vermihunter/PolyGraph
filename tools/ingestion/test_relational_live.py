"""Service-free checks for the relational acceptance runner, not live substitutes."""

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace

from tools.ingestion.relational_live import assert_distances, expected_failure, normalize_dump
from tools.ingestion.docker_environment import WorkflowError


class RelationalAcceptanceAssertionsTest(unittest.TestCase):
    def test_distance_oracle_rejects_missing_extra_duplicate_and_wrong_layers(self):
        rows = [{"vertex_id": "10", "distance": "0"}, {"vertex_id": "20", "distance": "1"}]
        assert_distances(rows, {10: 0, 20: 1})
        wrong = copy.deepcopy(rows)
        wrong[1]["distance"] = "2"
        for actual in (wrong, rows[:1], rows + [rows[0]], rows + [{"vertex_id": "30", "distance": "2"}]):
            with self.subTest(actual=actual), self.assertRaises(WorkflowError):
                assert_distances(actual, {10: 0, 20: 1})

    def test_dump_normalization_preserves_schema_and_data_changes(self):
        before = "\\restrict random1\nCREATE TABLE app.person(id bigint);\n7\n\\unrestrict random1\n"
        after = before.replace("random1", "random2")
        self.assertEqual(normalize_dump(before), normalize_dump(after))
        self.assertNotEqual(normalize_dump(before), normalize_dump(after.replace("bigint", "text")))
        self.assertNotEqual(normalize_dump(before), normalize_dump(after.replace("\n7\n", "\n8\n")))

    def test_negative_cases_require_the_intended_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "logs").mkdir()
            environment = SimpleNamespace(output=output, env=os.environ.copy(), state={}, save=lambda: None)
            expected_failure(environment, "matching", [sys.executable, "-c", "raise SystemExit('different mapping')"],
                             diagnostic="different mapping")
            with self.assertRaises(WorkflowError):
                expected_failure(environment, "wrong-reason", [sys.executable, "-c", "raise SystemExit('network unavailable')"],
                                 diagnostic="different mapping")
            with self.assertRaises(WorkflowError):
                expected_failure(environment, "unexpected-success", [sys.executable, "-c", "pass"],
                                 diagnostic="different mapping")

    def test_explicit_run_fails_instead_of_skipping_without_prerequisites(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run"
            result = subprocess.run([
                sys.executable, "tools/ingestion/relational_live.py", "--run",
                "--binary", str(Path(directory) / "missing"), "--bfs", "missing",
                "--attach", "missing", "--probe", "missing", "--output-dir", str(output),
            ], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotEqual(result.returncode, 77)
            report = json.loads((output / "integration-result.json").read_text())
            self.assertEqual(report["state"], "failed")
            self.assertFalse(report["cleanup"]["attempted"])

    def test_no_services_or_output_without_explicit_run(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run"
            result = subprocess.run([
                sys.executable, "tools/ingestion/relational_live.py", "--binary", "missing",
                "--bfs", "missing", "--attach", "missing", "--probe", "missing",
                "--output-dir", str(output),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
