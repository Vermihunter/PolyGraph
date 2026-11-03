from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import copy
import io
from pathlib import Path
import tempfile
import unittest

from .dataset import create_lock, file_digest, generate_queries, read_json, write_json
from .compare import read_result
from .ldbc_csv import load_csv_graph
from .run import main, run_queries


class OracleCommandTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.raw = self.root / "raw"
        (self.raw / "static").mkdir(parents=True)
        (self.raw / "dynamic").mkdir()
        (self.raw / "dynamic/person_0_0.csv").write_text("id\n1\n2\n3\n", encoding="utf-8")
        (self.raw / "dynamic/person_knows_person_0_0.csv").write_text(
            "Person.id|Person.id\n1|2\n2|3\n", encoding="utf-8"
        )
        self.lock = self.root / "lock.json"
        lock = create_lock(self.raw, require_complete=False)
        write_json(self.lock, lock)
        self.queries = self.root / "queries.json"
        queries = generate_queries(load_csv_graph(self.raw), lock)
        queries["queries"] = queries["queries"][:1]
        write_json(self.queries, queries)

    def test_run_writes_manually_verified_chain_and_detects_corruptions(self) -> None:
        out = self.root / "out"
        with redirect_stdout(io.StringIO()):
            report = run_queries(self.raw, self.lock, self.queries, out)
        self.assertEqual(report["query_count"], 1)
        self.assertEqual(report["results"][0]["reached_vertices"], 3)
        self.assertTrue(all(report["corruption_probes"].values()))
        self.assertEqual(set(report["corruption_probes"]),
                         {"missing_vertex", "extra_vertex", "wrong_distance", "invalid_predecessor", "duplicate_row"})
        result = out / report["results"][0]["file"]
        self.assertEqual(result.read_text(),
                         "vertex_type,vertex_id,distance,predecessor_type,predecessor_id\n"
                         "Person,1,0,,\nPerson,2,1,Person,1\nPerson,3,2,Person,2\n")
        self.assertEqual(read_json(out / "report.json"), report)
        with self.assertRaisesRegex(ValueError, "empty|exists"):
            run_queries(self.raw, self.lock, self.queries, out)

    def test_compare_cli_returns_failure_for_corrupted_distance(self) -> None:
        actual = self.root / "actual.csv"
        actual.write_text("vertex_type,vertex_id,distance,predecessor_type,predecessor_id\n"
                          "Person,1,0,,\nPerson,2,1,Person,1\nPerson,3,9,Person,2\n", encoding="utf-8")
        report = self.root / "comparison.json"
        name = read_json(self.queries)["queries"][0]["name"]
        args = ["compare", "--dataset", str(self.raw), "--lock", str(self.lock),
                "--queries", str(self.queries), "--query", name, "--actual", str(actual), "--report", str(report)]
        with redirect_stdout(io.StringIO()):
            self.assertEqual(main(args), 1)
        comparison = read_json(report)
        self.assertFalse(comparison["same"])
        self.assertEqual(comparison["wrong_distance"], [[["Person", 3], 2, 9]])

    def test_query_name_cannot_collide_with_corruption_artifacts(self) -> None:
        queries = read_json(self.queries)
        queries["queries"][0]["name"] = "deliberate-duplicate"
        write_json(self.queries, queries)
        output = self.root / "collision"
        with redirect_stdout(io.StringIO()):
            report = run_queries(self.raw, self.lock, self.queries, output)
        result = output / report["results"][0]["file"]
        self.assertEqual(file_digest(result), report["results"][0]["sha256"])
        self.assertEqual(read_result(result).distance, {("Person", 1): 0, ("Person", 2): 1, ("Person", 3): 2})

    def test_cli_rejects_duplicate_actual_rows_with_nonzero_status(self) -> None:
        actual = self.root / "duplicate.csv"
        actual.write_text("vertex_type,vertex_id,distance,predecessor_type,predecessor_id\n"
                          "Person,1,0,,\nPerson,1,0,,\n", encoding="utf-8")
        name = read_json(self.queries)["queries"][0]["name"]
        args = ["compare", "--dataset", str(self.raw), "--lock", str(self.lock),
                "--queries", str(self.queries), "--query", name, "--actual", str(actual)]
        with redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(main(args), 2)
        self.assertIn("duplicate", errors.getvalue())

    def test_empty_queries_wrong_lock_and_unsafe_names_fail_before_outputs(self) -> None:
        original = read_json(self.queries)
        for change in ("empty", "lock", "path"):
            with self.subTest(change=change):
                queries = copy.deepcopy(original)
                if change == "empty":
                    queries["queries"] = []
                elif change == "lock":
                    queries["dataset_lock_sha256"] = "0" * 64
                else:
                    queries["queries"][0]["name"] = "../escape"
                write_json(self.queries, queries)
                out = self.root / change
                with self.assertRaises(ValueError):
                    run_queries(self.raw, self.lock, self.queries, out)
                self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
