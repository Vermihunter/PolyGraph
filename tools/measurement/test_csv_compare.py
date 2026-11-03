from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path

from tools.measurement.graph_bench.csv_compare import compare_bfs_csv, read_bfs_csv, sorted_bfs_items


class CsvCompareTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def test_supported_headers_and_delimiters_preserve_uint64_precision(self) -> None:
        for delimiter, header in [
            (";", "vertex_id;distance"),
            (",", "uuid,distance"),
            (";", "UUID;Distance"),
            (",", "vertex,level"),
        ]:
            with self.subTest(header=header):
                source = self.root / "result.csv"
                source.write_text(
                    f"{header}\n18446744073709551614{delimiter}2\n7{delimiter}0\n{delimiter}\n",
                    encoding="utf-8",
                )

                rows = read_bfs_csv(source)

                self.assertEqual(rows, {"18446744073709551614": 2, "7": 0})
                self.assertEqual(sorted_bfs_items(rows), [("7", 0), ("18446744073709551614", 2)])

    def test_comparison_rejects_duplicate_vertices(self) -> None:
        for duplicate_side in ("expected", "actual"):
            for duplicate_distance in (1, 2):
                with self.subTest(side=duplicate_side, distance=duplicate_distance):
                    expected = self.root / "expected.csv"
                    actual = self.root / "actual.csv"
                    expected.write_text("vertex_id;distance\n7;1\n", encoding="utf-8")
                    actual.write_text("vertex_id;distance\n7;1\n", encoding="utf-8")
                    duplicate_file = expected if duplicate_side == "expected" else actual
                    duplicate_file.write_text(
                        f"vertex_id;distance\n7;1\n7;{duplicate_distance}\n", encoding="utf-8"
                    )

                    with self.assertRaisesRegex(ValueError, r"Duplicate.*7") as error:
                        compare_bfs_csv(expected, actual)

                    self.assertIn(duplicate_file.name, str(error.exception))

    def test_comparison_reports_missing_extra_and_wrong_distances(self) -> None:
        expected = self.root / "expected.csv"
        actual = self.root / "actual.csv"
        diff = self.root / "diffs" / "result.csv"
        expected.write_text("vertex_id;distance\n1;0\n2;1\n3;2\n", encoding="utf-8")
        actual.write_text("uuid,distance\n4,3\n3,1\n1,0\n", encoding="utf-8")

        result = compare_bfs_csv(expected, actual, diff)

        self.assertFalse(result.same)
        self.assertEqual((result.expected_count, result.actual_count), (3, 3))
        self.assertEqual((result.missing_count, result.extra_count, result.wrong_distance_count), (1, 1, 1))
        self.assertEqual(result.diff_csv, str(diff))
        with diff.open(newline="", encoding="utf-8") as handle:
            self.assertEqual(
                list(csv.DictReader(handle)),
                [
                    {"kind": "missing", "vertex_id": "2", "expected_distance": "1", "actual_distance": ""},
                    {"kind": "extra", "vertex_id": "4", "expected_distance": "", "actual_distance": "3"},
                    {"kind": "wrong_distance", "vertex_id": "3", "expected_distance": "2", "actual_distance": "1"},
                ],
            )

    def test_comparison_accepts_equal_results_in_different_row_orders(self) -> None:
        expected = self.root / "expected.csv"
        actual = self.root / "actual.csv"
        expected.write_text("vertex_id;distance\n1;0\n2;1\n", encoding="utf-8")
        actual.write_text("uuid,distance\n2,1\n1,0\n", encoding="utf-8")

        result = compare_bfs_csv(expected, actual)

        self.assertTrue(result.same)
        self.assertEqual((result.expected_count, result.actual_count), (2, 2))
        self.assertEqual((result.missing_count, result.extra_count, result.wrong_distance_count), (0, 0, 0))


if __name__ == "__main__":
    unittest.main()
