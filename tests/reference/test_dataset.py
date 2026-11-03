from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

from .dataset import create_lock, generate_queries, lock_digest, read_json, verify_lock
from .ldbc_csv import load_csv_graph


class DatasetLockTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "static").mkdir()
        (self.root / "dynamic").mkdir()
        self.nodes = self.root / "dynamic/person_0_0.csv"
        self.nodes.write_text("id|firstName\n1|Ada\n2|Bob\n3|Isolated\n", encoding="utf-8")
        (self.root / "dynamic/person_knows_person_0_0.csv").write_text(
            "Person.id|Person.id|creationDate\n1|2|0\n", encoding="utf-8"
        )
        (self.root / "params.ini").write_text("scaleFactor:snb.interactive.0.1\n", encoding="utf-8")
        self.lock = create_lock(self.root, require_complete=False)

    def test_lock_records_exact_content_counts_and_honest_provenance(self) -> None:
        self.assertEqual(self.lock["counts"]["vertices"], 3)
        self.assertEqual(self.lock["counts"]["relationships"], 1)
        self.assertEqual(len(self.lock["files"]), 3)
        self.assertIsNone(self.lock["provenance"]["generator_commit"])
        self.assertIsNone(self.lock["provenance"]["generator_seed"])
        self.assertEqual(len(lock_digest(self.lock)), 64)
        self.assertEqual(verify_lock(self.root, self.lock).vertices, load_csv_graph(self.root).vertices)

    def test_same_rows_with_changed_bytes_are_rejected(self) -> None:
        self.nodes.write_text("id|firstName\n1|Changed\n2|Bob\n3|Isolated\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "lock|checksum|inventory"):
            verify_lock(self.root, self.lock)

    def test_added_and_removed_shards_are_rejected(self) -> None:
        extra = self.root / "dynamic/person_1_0.csv"
        extra.write_text("id|firstName\n4|Extra\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            verify_lock(self.root, self.lock)
        extra.unlink()
        self.nodes.unlink()
        with self.assertRaises(ValueError):
            verify_lock(self.root, self.lock)

    def test_update_streams_are_not_part_of_the_initial_snapshot(self) -> None:
        (self.root / "updateStream_0_0_person.csv").write_text("not snapshot CSV\n", encoding="utf-8")
        self.assertEqual(create_lock(self.root, require_complete=False), self.lock)

    def test_tampered_counts_or_duplicate_inventory_entries_are_rejected(self) -> None:
        changed = copy.deepcopy(self.lock)
        changed["counts"]["vertices"] = 4
        with self.assertRaises(ValueError):
            verify_lock(self.root, changed)
        changed = copy.deepcopy(self.lock)
        changed["files"].append(changed["files"][0])
        with self.assertRaises(ValueError):
            verify_lock(self.root, changed)

    def test_duplicate_json_keys_are_rejected(self) -> None:
        path = self.root / "bad.json"
        path.write_text('{"version":1,"version":2}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            read_json(path)

    def test_nonfinite_json_and_numerically_equal_float_counts_are_rejected(self) -> None:
        path = self.root / "nonfinite.json"
        for value in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(value=value):
                path.write_text('{"value":' + value + '}', encoding="utf-8")
                with self.assertRaises(ValueError):
                    read_json(path)
        changed = copy.deepcopy(self.lock)
        changed["counts"]["vertices"] = 3.0
        with self.assertRaises(ValueError):
            verify_lock(self.root, changed)

    def test_roots_and_queries_are_fixed_unique_and_bound_to_lock(self) -> None:
        graph = load_csv_graph(self.root)
        queries = generate_queries(graph, self.lock)
        self.assertEqual(queries, generate_queries(graph, self.lock))
        self.assertEqual(queries["dataset_lock_sha256"], lock_digest(self.lock))
        keys = {(r["vertex_type"], int(r["vertex_id"])) for r in queries["roots"]}
        self.assertEqual(keys, {("Person", 1), ("Person", 2), ("Person", 3)})
        self.assertEqual(len(keys), len(queries["roots"]))
        self.assertTrue(any(q["max_depth"] == 0 for q in queries["queries"]))
        self.assertTrue(any(q["max_depth"] is None for q in queries["queries"]))
        self.assertEqual(json.loads(json.dumps(queries)), queries)


if __name__ == "__main__":
    unittest.main()
