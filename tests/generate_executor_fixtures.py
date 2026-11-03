"""Build-time bridge to the unchanged Phase 2 oracle; never import production code.

IDs in this file are small logical fixture indices, independent of routing IDs.
C++ alone encodes ownership when presenting the same graph to the executor.
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

from tests.reference.bfs import bfs, project
from tests.reference.model import Edge, Graph

TYPES = {1: "Person", 2: "Place"}
RELATIONS = {7: "PERSON_KNOWS_PERSON", 8: "PERSON_ISLOCATEDIN_PLACE"}


def fixtures() -> list[dict]:
    # Reversed edges and mixed types make destination filtering observable in both directions.
    topologies = {
        "chain": ([1] * 6, [(i, i + 1, 7) for i in range(5)]),
        "diamond": ([1] * 5, [(0, 1, 7), (0, 2, 7), (1, 3, 7), (2, 3, 7), (3, 4, 7)]),
        "cycle": ([1] * 4, [(0, 1, 7), (1, 2, 7), (2, 0, 7), (2, 3, 7)]),
        "disconnected": ([1] * 6, [(0, 1, 7), (1, 2, 7), (3, 4, 7)]),
        "selfloop": ([1] * 3, [(0, 0, 7), (0, 1, 7), (1, 1, 7), (1, 2, 7), (2, 2, 7)]),
        "duplicates": ([1] * 4, [(0, 1, 7)] * 3 + [(0, 2, 7), (1, 3, 7), (2, 3, 7)] * 2),
        "filters": ([1, 2, 1, 2, 1, 2], [(0, 1, 8), (2, 0, 7), (0, 3, 7), (1, 4, 7), (3, 5, 8), (5, 2, 8)]),
        # Frontier widths 1,4,1,1,1: deterministic push -> pull -> push.
        "switching": ([1] * 8, [(0, i, 7) for i in range(1, 5)] + [(i, 5, 7) for i in range(1, 5)] + [(5, 6, 7), (6, 7, 7)]),
        "isolated": ([1], []),
    }
    cases = []
    for name, (types, edges) in topologies.items():
        vertices = [(TYPES[t], i) for i, t in enumerate(types)]
        graph = Graph(frozenset(vertices), tuple(Edge(vertices[a], vertices[b], RELATIONS[r]) for a, b, r in edges))
        for direction in ("outgoing", "both"):
            for filter_name, edge_types, target_types in (
                ("all", [], []), ("relation", [7], []),
                ("target", [], [2]), ("combined", [8], [1]),
            ):
                projection = project(graph, direction=direction,
                                     edge_types=[RELATIONS[r] for r in edge_types],
                                     target_types=[TYPES[t] for t in target_types])
                for depth in (0, 1, 2, None):
                    result = bfs(projection, vertices[0], max_depth=depth)
                    cases.append(dict(
                        name=f"{name}_{direction}_{filter_name}_{'unlimited' if depth is None else depth}",
                        topology=name, types=types, edges=edges, direction=direction,
                        edge_types=edge_types, target_types=target_types, max_depth=depth,
                        distance={str(v[1]): d for v, d in result.distance.items()},
                        # Full projected adjacency permits *any* shortest-path parent, independent of order.
                        adjacency={str(v[1]): [n[1] for n in neighbors] for v, neighbors in projection.adjacency.items()},
                    ))
    return cases


if __name__ == "__main__":
    Path(sys.argv[1]).write_text(json.dumps(fixtures(), indent=2) + "\n", encoding="utf-8")
