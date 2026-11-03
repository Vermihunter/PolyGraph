"""Hand-checked and independent all-pairs tests for the test-only BFS oracle."""

from __future__ import annotations

import random
import unittest

from tests.reference.bfs import bfs, project
from tests.reference.model import Edge, Graph, Projection


KNOWS = "PERSON_KNOWS_PERSON"
LOCATED = "PERSON_ISLOCATEDIN_PLACE"
HAS_CREATOR = "POST_HASCREATOR_PERSON"


class ModelTests(unittest.TestCase):
    def test_all_eight_types_and_uint64_limits_are_distinct_vertices(self) -> None:
        vertices = frozenset(
            (name, raw_id)
            for name in (
                "Comment", "Forum", "Organisation", "Person", "Place", "Post", "Tag", "TagClass"
            )
            for raw_id in (0, (1 << 64) - 1)
        )
        graph = Graph(vertices, ())
        self.assertEqual(len(graph.vertices), 16)

    def test_invalid_typed_ids_are_rejected(self) -> None:
        for vertex in (
            ("person", 1), ("Unknown", 1), ("Person", -1), ("Person", 1 << 64),
            ("Person", True), ("Person", 1.0), ("Person", "1"),
            ("Person",), ("Person", 1, 2), "Person:1", (1, 1),
        ):
            with self.subTest(vertex=vertex), self.assertRaises((ValueError, TypeError)):
                Graph(frozenset((vertex,)), ())

    def test_dangling_edge_source_and_target_are_rejected(self) -> None:
        root, absent = ("Person", 1), ("Person", 2)
        for edge in (Edge(root, absent, KNOWS), Edge(absent, root, KNOWS)):
            with self.subTest(edge=edge), self.assertRaises(ValueError):
                Graph(frozenset((root,)), (edge,))

    def test_invalid_edge_endpoint_ids_are_rejected(self) -> None:
        root = ("Person", 1)
        with self.assertRaises((ValueError, TypeError)):
            Graph(frozenset((root,)), (Edge(root, ("Person", False), KNOWS),))

    def test_non_string_edge_relation_is_rejected(self) -> None:
        root = ("Person", 1)
        with self.assertRaises((ValueError, TypeError)):
            Graph(frozenset((root,)), (Edge(root, root, 7),))


class ProjectionTests(unittest.TestCase):
    def test_outgoing_uses_csv_orientation_even_for_knows(self) -> None:
        one, two = ("Person", 1), ("Person", 2)
        projection = project(Graph(frozenset((one, two)), (Edge(one, two, KNOWS),)), direction="outgoing")
        self.assertEqual(projection.adjacency, {one: (two,), two: ()})
        self.assertEqual(bfs(projection, two).distance, {two: 0})

    def test_default_both_adds_reverse_for_every_relation(self) -> None:
        person, place = ("Person", 1), ("Place", 2)
        graph = Graph(frozenset((person, place)), (Edge(person, place, LOCATED),))
        self.assertEqual(project(graph).adjacency, {person: (place,), place: (person,)})

    def test_edge_filter_applies_before_traversal_union(self) -> None:
        one, two, three = ("Person", 1), ("Person", 2), ("Place", 3)
        graph = Graph(frozenset((one, two, three)), (Edge(one, two, KNOWS), Edge(two, three, LOCATED)))
        projection = project(graph, edge_types=(KNOWS,))
        self.assertEqual(projection.adjacency, {one: (two,), two: (one,), three: ()})
        self.assertEqual(bfs(projection, one).distance, {one: 0, two: 1})

    def test_known_absent_edge_filter_gives_empty_transitions(self) -> None:
        one, two = ("Person", 1), ("Person", 2)
        graph = Graph(frozenset((one, two)), (Edge(one, two, KNOWS),))
        self.assertEqual(project(graph, edge_types=(LOCATED,)).adjacency, {one: (), two: ()})

    def test_target_filter_prunes_intermediate_vertices(self) -> None:
        one, place, two = ("Person", 1), ("Place", 2), ("Person", 3)
        graph = Graph(frozenset((one, place, two)), (Edge(one, place, LOCATED), Edge(two, place, LOCATED)))
        projection = project(graph, target_types=("Person",))
        self.assertEqual(projection.vertices, graph.vertices)
        self.assertEqual(projection.adjacency, {one: (), place: (one, two), two: ()})
        self.assertEqual(bfs(projection, one).distance, {one: 0})

    def test_source_is_retained_when_its_type_is_not_a_target_type(self) -> None:
        person, place = ("Person", 1), ("Place", 2)
        graph = Graph(frozenset((person, place)), (Edge(person, place, LOCATED),))
        projection = project(graph, target_types=("Place",))
        self.assertEqual(bfs(projection, person).distance, {person: 0, place: 1})
        self.assertEqual(bfs(projection, place).distance, {place: 0})

    def test_neighbors_are_unique_and_sorted_by_type_then_numeric_id(self) -> None:
        root, small, large, place = ("Person", 0), ("Person", 2), ("Person", 10), ("Place", 1)
        graph = Graph(
            frozenset((root, small, large, place)),
            (Edge(root, place, LOCATED), Edge(root, large, KNOWS), Edge(root, small, KNOWS),
             Edge(root, large, KNOWS), Edge(root, root, KNOWS)),
        )
        self.assertEqual(project(graph, direction="outgoing").adjacency[root], (root, small, large, place))

    def test_invalid_direction_is_rejected_even_for_empty_graph(self) -> None:
        for direction in ("incoming", "Both", "", None, True):
            with self.subTest(direction=direction), self.assertRaises((ValueError, TypeError)):
                project(Graph(frozenset(), ()), direction=direction)

    def test_invalid_filters_are_rejected_even_for_empty_graph(self) -> None:
        for keyword, value in (
            ("edge_types", ("UNKNOWN",)), ("target_types", ("person",)),
            ("edge_types", KNOWS), ("target_types", "Person"),
            ("edge_types", (False,)), ("target_types", (7,)),
            ("edge_types", None), ("target_types", None),
        ):
            with self.subTest(keyword=keyword, value=value), self.assertRaises((ValueError, TypeError)):
                project(Graph(frozenset(), ()), **{keyword: value})


class BfsTests(unittest.TestCase):
    def test_chain_depth_is_inclusive_and_unbounded_by_default(self) -> None:
        one, two, three, four = (("Person", number) for number in (1, 2, 3, 4))
        projection = project(Graph(frozenset((one, two, three, four)), (
            Edge(one, two, KNOWS), Edge(two, three, KNOWS), Edge(three, four, KNOWS),
        )), direction="outgoing")
        for depth, distance, predecessor in (
            (0, {one: 0}, {}),
            (1, {one: 0, two: 1}, {two: one}),
            (2, {one: 0, two: 1, three: 2}, {two: one, three: two}),
            (3, {one: 0, two: 1, three: 2, four: 3}, {two: one, three: two, four: three}),
            (None, {one: 0, two: 1, three: 2, four: 3}, {two: one, three: two, four: three}),
        ):
            with self.subTest(depth=depth):
                result = bfs(projection, one, max_depth=depth)
                self.assertEqual(result.distance, distance)
                self.assertEqual(result.predecessor, predecessor)

    def test_boundary_vertices_are_not_expanded(self) -> None:
        one, two = ("Person", 1), ("Person", 2)

        class Adjacency(dict):
            def __getitem__(self, vertex):
                if vertex == two:
                    raise AssertionError("depth boundary was expanded")
                return super().__getitem__(vertex)

            def get(self, vertex, default=None):
                if vertex == two:
                    raise AssertionError("depth boundary was expanded")
                return super().get(vertex, default)

        result = bfs(Projection(frozenset((one, two)), Adjacency({one: (two,), two: (one,)})), one, max_depth=1)
        self.assertEqual(result.distance, {one: 0, two: 1})

    def test_zero_depth_does_not_read_any_adjacency(self) -> None:
        root = ("Person", 1)

        class Adjacency(dict):
            def __getitem__(self, vertex):
                raise AssertionError("zero-depth root was expanded")

            def get(self, vertex, default=None):
                raise AssertionError("zero-depth root was expanded")

        result = bfs(Projection(frozenset((root,)), Adjacency()), root, max_depth=0)
        self.assertEqual(result.distance, {root: 0})

    def test_diamond_selects_first_sorted_parent_at_enqueue(self) -> None:
        root, left, right, leaf = (("Person", number) for number in (1, 2, 3, 4))
        graph = Graph(frozenset((root, left, right, leaf)), (
            Edge(right, leaf, KNOWS), Edge(root, right, KNOWS),
            Edge(left, leaf, KNOWS), Edge(root, left, KNOWS),
        ))
        result = bfs(project(graph, direction="outgoing"), root)
        self.assertEqual(result.distance, {root: 0, left: 1, right: 1, leaf: 2})
        self.assertEqual(result.predecessor, {left: root, right: root, leaf: left})

    def test_fifo_traversal_keeps_shortest_path_instead_of_first_depth_first_path(self) -> None:
        root, a, b, c, target = (("Person", number) for number in (0, 1, 2, 3, 4))
        graph = Graph(frozenset((root, a, b, c, target)), (
            Edge(root, a, KNOWS), Edge(a, b, KNOWS), Edge(b, target, KNOWS),
            Edge(root, c, KNOWS), Edge(c, target, KNOWS),
        ))
        result = bfs(project(graph, direction="outgoing"), root)
        self.assertEqual(result.distance, {root: 0, a: 1, c: 1, b: 2, target: 2})
        self.assertEqual(result.predecessor[target], c)

    def test_cycles_self_loops_and_duplicate_edges_do_not_revisit_source(self) -> None:
        one, two, three = (("Person", number) for number in (1, 2, 3))
        graph = Graph(frozenset((one, two, three)), (
            Edge(one, one, KNOWS), Edge(one, two, KNOWS), Edge(one, two, KNOWS),
            Edge(two, three, KNOWS), Edge(three, one, KNOWS),
        ))
        result = bfs(project(graph, direction="outgoing"), one)
        self.assertEqual(result.distance, {one: 0, two: 1, three: 2})
        self.assertEqual(result.predecessor, {two: one, three: two})

    def test_disconnected_components_and_isolated_root(self) -> None:
        one, two, three, four, isolated = (("Person", number) for number in (1, 2, 3, 4, 5))
        graph = Graph(frozenset((one, two, three, four, isolated)), (
            Edge(one, two, KNOWS), Edge(three, four, KNOWS),
        ))
        self.assertEqual(bfs(project(graph), one).distance, {one: 0, two: 1})
        result = bfs(project(graph), isolated)
        self.assertEqual(result.distance, {isolated: 0})
        self.assertEqual(result.predecessor, {})

    def test_large_numeric_ids_and_equal_raw_ids_of_different_types(self) -> None:
        person, post, place = ("Person", (1 << 64) - 1), ("Post", (1 << 64) - 1), ("Place", 1 << 63)
        graph = Graph(frozenset((person, post, place)), (
            Edge(post, person, HAS_CREATOR), Edge(person, place, LOCATED),
        ))
        result = bfs(project(graph), post)
        self.assertEqual(result.distance, {post: 0, person: 1, place: 2})
        self.assertEqual(result.predecessor, {person: post, place: person})

    def test_predecessors_can_be_disabled_without_changing_distances(self) -> None:
        one, two = ("Person", 1), ("Person", 2)
        result = bfs(project(Graph(frozenset((one, two)), (Edge(one, two, KNOWS),))), one, return_predecessors=False)
        self.assertEqual(result.distance, {one: 0, two: 1})
        self.assertEqual(result.predecessor, {})

    def test_missing_or_invalid_root_is_rejected(self) -> None:
        projection = project(Graph(frozenset((("Person", 1),)), ()))
        for source in (("Person", 2), ("person", 1), ("Person", True), ("Person", "1")):
            with self.subTest(source=source), self.assertRaises((ValueError, TypeError)):
                bfs(projection, source)

    def test_invalid_depth_is_rejected(self) -> None:
        root = ("Person", 1)
        projection = project(Graph(frozenset((root,)), ()))
        for depth in (-1, True, False, 1.0, "1"):
            with self.subTest(depth=depth), self.assertRaises((ValueError, TypeError)):
                bfs(projection, root, max_depth=depth)


class FloydWarshallCrossCheckTests(unittest.TestCase):
    def test_seeded_tiny_graphs_match_independent_all_pairs_distances(self) -> None:
        """A matrix shortest-path algorithm checks BFS on generated small graphs."""
        rng = random.Random(81273)
        vertices = (("Person", 0), ("Person", 1), ("Place", 0), ("Post", (1 << 64) - 1))
        infinity = len(vertices) + 1
        for case in range(16):
            edges = tuple(
                Edge(source, target, relation)
                for source in vertices
                for target in vertices
                for relation in (KNOWS, LOCATED)
                if rng.randrange(5) == 0
            )
            graph = Graph(frozenset(vertices), edges)
            for direction in ("outgoing", "both"):
                for edge_types in ((), (KNOWS,)):
                    for target_types in ((), ("Person",), ("Place", "Post")):
                        # Construct the matrix from edge rows, never from project()'s adjacency.
                        matrix = [[0 if i == j else infinity for j in range(4)] for i in range(4)]
                        for edge in edges:
                            if edge_types and edge.relation not in edge_types:
                                continue
                            orientations = ((edge.source, edge.target),)
                            if direction == "both":
                                orientations += ((edge.target, edge.source),)
                            for source, target in orientations:
                                if not target_types or target[0] in target_types:
                                    i, j = vertices.index(source), vertices.index(target)
                                    matrix[i][j] = min(matrix[i][j], 1)
                        for middle in range(4):
                            for start in range(4):
                                for end in range(4):
                                    matrix[start][end] = min(
                                        matrix[start][end], matrix[start][middle] + matrix[middle][end]
                                    )
                        projection = project(
                            graph, direction=direction, edge_types=edge_types, target_types=target_types
                        )
                        for start, source in enumerate(vertices):
                            for depth in (None, 0, 1, 2):
                                expected = {
                                    vertex: matrix[start][end]
                                    for end, vertex in enumerate(vertices)
                                    if matrix[start][end] < infinity
                                    and (depth is None or matrix[start][end] <= depth)
                                }
                                with self.subTest(case=case, direction=direction, edge_types=edge_types,
                                                  target_types=target_types, source=source, depth=depth):
                                    result = bfs(projection, source, max_depth=depth)
                                    self.assertEqual(result.distance, expected)
                                    self.assertEqual(set(result.predecessor), set(expected) - {source})
                                    for child, parent in result.predecessor.items():
                                        self.assertIn(child, projection.adjacency[parent])
                                        self.assertEqual(result.distance[parent] + 1, result.distance[child])


if __name__ == "__main__":
    unittest.main()
