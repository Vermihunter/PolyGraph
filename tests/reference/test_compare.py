"""Hand-derived checks for strict, independent BFS result comparison."""

from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from .compare import Comparison, compare_results, read_result, write_result
from .model import BfsResult, Projection


SOURCE = ("Person", 1)
LEFT = ("Person", 2)
RIGHT = ("Person", 3)
LEAF = ("Post", 4)
ISOLATED = ("Tag", 9)
HIGH = ("Person", 9007199254740993)
MAX_ID = 18446744073709551615


class ResultCsvTests(unittest.TestCase):
    def setUp(self):
        self.directory = TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "result.csv"

    def read(self, contents, **kwargs):
        self.path.write_text(contents, encoding="utf-8")
        return read_result(self.path, **kwargs)

    def test_reads_shuffled_typed_rows_without_float_conversion(self):
        result = self.read(
            "vertex_type,vertex_id,distance,predecessor_type,predecessor_id\n"
            "Post,1,1,Person,9007199254740993\n"
            "Person,18446744073709551615,2,Post,1\n"
            "Person,9007199254740993,0,,\n"
            "Person,1,1,Person,9007199254740993\n",
            require_predecessors=True,
        )
        self.assertEqual(result.distance, {
            ("Person", 9007199254740993): 0,
            ("Person", MAX_ID): 2,
            ("Post", 1): 1,
            ("Person", 1): 1,
        })
        self.assertEqual(result.predecessor, {
            ("Person", MAX_ID): ("Post", 1),
            ("Post", 1): ("Person", 9007199254740993),
            ("Person", 1): ("Person", 9007199254740993),
        })

    def test_basic_header_requires_opt_out_from_predecessor_requirement(self):
        contents = "vertex_type,vertex_id,distance\nPerson,1,0\n"
        self.assertEqual(self.read(contents).distance, {SOURCE: 0})
        self.assertEqual(self.read(contents).predecessor, {})
        with self.assertRaises(ValueError):
            self.read(contents, require_predecessors=True)

    def test_rejects_missing_reordered_duplicate_and_extra_headers(self):
        invalid_headers = (
            "", "vertex_type,vertex_id\n", "vertex_id,vertex_type,distance\n",
            "vertex_type,vertex_id,distance,distance\n",
            "vertex_type,vertex_id,distance,predecessor_id,predecessor_type\n",
            "vertex_type,vertex_id,distance,predecessor_type\n",
            "vertex_type,vertex_id,distance,predecessor_type,predecessor_id,extra\n",
            " vertex_type,vertex_id,distance\n",
        )
        for contents in invalid_headers:
            with self.subTest(contents=contents), self.assertRaises(ValueError):
                self.read(contents)

    def test_rejects_short_extra_blank_and_malformed_csv_rows(self):
        for row in ("Person,1\n", "Person,1,0,extra\n", "\n", ",,\n", '"Person,1,0\n'):
            with self.subTest(row=row), self.assertRaises(ValueError):
                self.read("vertex_type,vertex_id,distance\n" + row)

    def test_rejects_noncanonical_unknown_and_overflow_vertex_ids(self):
        invalid = (
            ("person", "1"), ("Unknown", "1"), (" Person", "1"),
            ("Person", ""), ("Person", "-1"), ("Person", "+1"),
            ("Person", "01"), ("Person", "1.0"), ("Person", "1e0"),
            ("Person", "True"), ("Person", " 1"), ("Person", "١"),
            ("Person", "18446744073709551616"),
        )
        for vertex_type, vertex_id in invalid:
            with self.subTest(vertex_type=vertex_type, vertex_id=vertex_id), self.assertRaises(ValueError):
                self.read(f"vertex_type,vertex_id,distance\n{vertex_type},{vertex_id},0\n")

    def test_rejects_negative_noncanonical_and_noninteger_distances(self):
        for value in ("", "-1", "+1", "00", "01", "1.0", "1e0", "True", " 1", "١"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.read(f"vertex_type,vertex_id,distance\nPerson,1,{value}\n")

    def test_rejects_repeated_logical_vertices_even_with_identical_distance(self):
        for second_distance in (0, 1):
            with self.subTest(second_distance=second_distance), self.assertRaises(ValueError):
                self.read(
                    "vertex_type,vertex_id,distance\n"
                    f"Person,1,0\nPerson,1,{second_distance}\n"
                )

    def test_rejects_half_empty_or_malformed_predecessor_pairs(self):
        for parent_type, parent_id in (("Person", ""), ("", "1"), ("person", "1"),
                                       ("Person", "01"), ("Person", "18446744073709551616")):
            with self.subTest(parent_type=parent_type, parent_id=parent_id), self.assertRaises(ValueError):
                self.read(
                    "vertex_type,vertex_id,distance,predecessor_type,predecessor_id\n"
                    f"Person,1,0,{parent_type},{parent_id}\n"
                )

    def test_writes_sorted_numeric_rows_with_fixed_newlines(self):
        result = BfsResult(
            distance={("Person", 10): 1, ("Post", 1): 1, SOURCE: 0, LEFT: 1, HIGH: 2},
            predecessor={LEFT: SOURCE, ("Person", 10): SOURCE, ("Post", 1): SOURCE, HIGH: LEFT},
        )
        write_result(result, self.path)
        self.assertEqual(self.path.read_bytes(), (
            "vertex_type,vertex_id,distance,predecessor_type,predecessor_id\n"
            "Person,1,0,,\n"
            "Person,2,1,Person,1\n"
            "Person,10,1,Person,1\n"
            "Post,1,1,Person,1\n"
            "Person,9007199254740993,2,Person,2\n"
        ).encode("utf-8"))

    def test_can_write_distance_only_without_mutating_the_result(self):
        result = BfsResult(distance={LEFT: 1, SOURCE: 0}, predecessor={LEFT: SOURCE})
        write_result(result, self.path, include_predecessors=False)
        self.assertEqual(self.path.read_bytes(), b"vertex_type,vertex_id,distance\nPerson,1,0\nPerson,2,1\n")
        self.assertEqual(result.predecessor, {LEFT: SOURCE})

    def test_writer_rejects_malformed_programmatic_numbers(self):
        invalid_results = (
            BfsResult(distance={("Person", True): 0}, predecessor={}),
            BfsResult(distance={("Person", 1.0): 0}, predecessor={}),
            BfsResult(distance={("Person", "1"): 0}, predecessor={}),
            BfsResult(distance={("Person", MAX_ID + 1): 0}, predecessor={}),
            BfsResult(distance={SOURCE: True}, predecessor={}),
            BfsResult(distance={SOURCE: 0.0}, predecessor={}),
            BfsResult(distance={SOURCE: -1}, predecessor={}),
            BfsResult(distance={SOURCE: 0}, predecessor={SOURCE: ("Person", True)}),
            BfsResult(distance={SOURCE: 0}, predecessor={LEFT: SOURCE}),
        )
        for result in invalid_results:
            with self.subTest(result=result), self.assertRaises(ValueError):
                write_result(result, self.path)


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        # The diamond allows either shortest parent of Post 4. Tag 9 is isolated.
        self.projection = Projection(
            vertices=frozenset((SOURCE, LEFT, RIGHT, LEAF, ISOLATED)),
            adjacency={SOURCE: (LEFT, RIGHT), LEFT: (LEAF,), RIGHT: (LEAF,), LEAF: (), ISOLATED: ()},
        )
        self.expected = BfsResult(
            distance={SOURCE: 0, LEFT: 1, RIGHT: 1, LEAF: 2},
            predecessor={LEFT: SOURCE, RIGHT: SOURCE, LEAF: LEFT},
        )

    def compare(self, actual, **kwargs):
        return compare_results(self.expected, actual, self.projection, SOURCE, **kwargs)

    def assert_reason(self, comparison, vertex, fragment):
        self.assertFalse(comparison.same)
        self.assertTrue(any(v == vertex and fragment in reason.lower()
                            for v, reason in comparison.invalid_predecessor),
                        comparison.invalid_predecessor)

    def test_accepts_reordered_rows_and_alternate_valid_shortest_parent(self):
        actual = BfsResult(
            distance={LEAF: 2, RIGHT: 1, SOURCE: 0, LEFT: 1},
            predecessor={LEAF: RIGHT, LEFT: SOURCE, RIGHT: SOURCE},
        )
        self.assertEqual(self.compare(actual), Comparison((), (), (), ()))
        self.assertTrue(self.compare(actual).same)

    def test_reports_literal_missing_extra_and_wrong_distances(self):
        actual = BfsResult(
            distance={SOURCE: 0, LEFT: 1, LEAF: 3, ISOLATED: 1},
            predecessor={LEFT: SOURCE, LEAF: LEFT, ISOLATED: SOURCE},
        )
        comparison = self.compare(actual)
        self.assertEqual(comparison.missing, (RIGHT,))
        self.assertEqual(comparison.extra, (ISOLATED,))
        self.assertEqual(comparison.wrong_distance, ((LEAF, 2, 3),))
        self.assertFalse(comparison.same)

    def test_rejects_missing_non_source_predecessor(self):
        actual = BfsResult(distance=dict(self.expected.distance), predecessor={LEFT: SOURCE, RIGHT: SOURCE})
        self.assert_reason(self.compare(actual), LEAF, "missing")

    def test_rejects_predecessor_on_source(self):
        actual = BfsResult(distance=dict(self.expected.distance), predecessor={**self.expected.predecessor, SOURCE: LEFT})
        self.assert_reason(self.compare(actual), SOURCE, "source")

    def test_rejects_foreign_predecessor_map_entry(self):
        actual = BfsResult(distance=dict(self.expected.distance), predecessor={**self.expected.predecessor, ISOLATED: SOURCE})
        self.assert_reason(self.compare(actual), ISOLATED, "unreached")

    def test_rejects_unknown_and_unreached_parents(self):
        for parent in (("Person", 99), ISOLATED):
            actual = BfsResult(distance=dict(self.expected.distance), predecessor={**self.expected.predecessor, LEAF: parent})
            with self.subTest(parent=parent):
                self.assert_reason(self.compare(actual), LEAF, "parent")

    def test_rejects_reverse_edges_and_parent_cycles(self):
        reverse = BfsResult(distance=dict(self.expected.distance), predecessor={**self.expected.predecessor, LEFT: LEAF})
        self.assert_reason(self.compare(reverse), LEFT, "edge")
        cycle = BfsResult(distance=dict(self.expected.distance), predecessor={LEFT: RIGHT, RIGHT: LEFT, LEAF: LEFT})
        self.assert_reason(self.compare(cycle), LEFT, "distance")
        self.assert_reason(self.compare(cycle), RIGHT, "distance")

    def test_rejects_parent_with_wrong_distance_even_when_edge_exists(self):
        actual = BfsResult(distance={SOURCE: 0, LEFT: 1, RIGHT: 1, LEAF: 1}, predecessor=dict(self.expected.predecessor))
        self.assert_reason(self.compare(actual), LEAF, "distance")

    def test_requires_source_distance_zero(self):
        actual = BfsResult(distance={SOURCE: 1, LEFT: 1, RIGHT: 1, LEAF: 2}, predecessor=dict(self.expected.predecessor))
        comparison = self.compare(actual)
        self.assertEqual(comparison.wrong_distance, ((SOURCE, 0, 1),))
        self.assert_reason(comparison, SOURCE, "zero")

    def test_rejects_actual_vertices_outside_graph_universe(self):
        unknown = ("Person", 99)
        actual = BfsResult(distance={**self.expected.distance, unknown: 1}, predecessor={**self.expected.predecessor, unknown: SOURCE})
        self.assertEqual(self.compare(actual).extra, (unknown,))
        self.assert_reason(self.compare(actual), unknown, "graph")

    def test_depth_limit_includes_boundary_and_rejects_beyond_boundary(self):
        expected = BfsResult(distance={SOURCE: 0, LEFT: 1, RIGHT: 1}, predecessor={LEFT: SOURCE, RIGHT: SOURCE})
        good = BfsResult(distance={RIGHT: 1, LEFT: 1, SOURCE: 0}, predecessor={RIGHT: SOURCE, LEFT: SOURCE})
        self.assertTrue(compare_results(expected, good, self.projection, SOURCE, max_depth=1).same)
        comparison = compare_results(expected, self.expected, self.projection, SOURCE, max_depth=1)
        self.assertEqual(comparison.extra, (LEAF,))
        self.assert_reason(comparison, LEAF, "depth")
        root_only = BfsResult(distance={SOURCE: 0}, predecessor={})
        self.assertTrue(compare_results(root_only, root_only, self.projection, SOURCE, max_depth=0).same)

    def test_distance_only_mode_rejects_any_actual_parent_map(self):
        no_parents = BfsResult(distance=dict(self.expected.distance), predecessor={})
        self.assertTrue(self.compare(no_parents, require_predecessors=False).same)
        self.assert_reason(self.compare(self.expected, require_predecessors=False), LEFT, "disabled")

    def test_high_ids_and_same_numeric_id_of_different_types_stay_distinct(self):
        root = ("Person", MAX_ID)
        person = ("Person", 9007199254740993)
        post = ("Post", 9007199254740993)
        graph = Projection(vertices=frozenset((root, person, post)), adjacency={root: (person, post), person: (), post: ()})
        expected = BfsResult(distance={root: 0, person: 1, post: 1}, predecessor={person: root, post: root})
        actual = BfsResult(distance={post: 1, root: 0, person: 1}, predecessor={post: root, person: root})
        self.assertTrue(compare_results(expected, actual, graph, root).same)
        missing_post = BfsResult(distance={root: 0, person: 1}, predecessor={person: root})
        self.assertEqual(compare_results(expected, missing_post, graph, root).missing, (post,))

    def test_high_degree_parent_validation_uses_linear_vertex_comparisons(self):
        class CountedVertex(tuple):
            comparisons = 0
            __hash__ = tuple.__hash__

            def __eq__(self, other):
                type(self).comparisons += 1
                return tuple.__eq__(self, other)

        # A single parent with 5,000 children must not rescan all earlier
        # neighbors for every predecessor witness. Count real equality work
        # rather than making the regression depend on machine speed.
        degree = 5000
        children = tuple(CountedVertex(("Person", identifier)) for identifier in range(2, degree + 2))
        graph = Projection(vertices=frozenset((SOURCE, *children)), adjacency={SOURCE: children})
        result = BfsResult(distance={SOURCE: 0, **dict.fromkeys(children, 1)},
                           predecessor=dict.fromkeys(children, SOURCE))
        CountedVertex.comparisons = 0
        self.assertTrue(compare_results(result, result, graph, SOURCE).same)
        self.assertLess(CountedVertex.comparisons, degree * 20)

    def test_malformed_programmatic_ids_and_distances_raise_value_error(self):
        for distance in (True, 1.0, "1", -1):
            actual = BfsResult(distance={**self.expected.distance, LEFT: distance}, predecessor=dict(self.expected.predecessor))
            with self.subTest(distance=distance), self.assertRaises(ValueError):
                self.compare(actual)
        for vertex in (("Person", True), ("Person", 1.0), ("Person", "1"), ("Person", -1),
                       ("Person", MAX_ID + 1), ("person", 1)):
            actual = BfsResult(distance={vertex: 0}, predecessor={})
            with self.subTest(vertex=vertex), self.assertRaises(ValueError):
                self.compare(actual)
        actual = BfsResult(distance=dict(self.expected.distance), predecessor={LEFT: ("Person", True)})
        with self.assertRaises(ValueError):
            self.compare(actual)

    def test_rejects_invalid_programmatic_reference_graph_source_and_depth(self):
        for source in (("Person", True), ("Person", 99)):
            with self.subTest(source=source), self.assertRaises(ValueError):
                compare_results(self.expected, self.expected, self.projection, source)
        for max_depth in (True, -1, 1.0, "1"):
            with self.subTest(max_depth=max_depth), self.assertRaises(ValueError):
                self.compare(self.expected, max_depth=max_depth)
        malformed = BfsResult(distance={SOURCE: True}, predecessor={})
        with self.assertRaises(ValueError):
            compare_results(malformed, self.expected, self.projection, SOURCE)
        graph = Projection(vertices=frozenset((SOURCE,)), adjacency={SOURCE: (("Person", True),)})
        with self.assertRaises(ValueError):
            compare_results(self.expected, self.expected, graph, SOURCE)


if __name__ == "__main__":
    unittest.main()
