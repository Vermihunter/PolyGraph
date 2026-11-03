from __future__ import annotations

import json
from pathlib import Path

from tools.algorithms.bfs import load_ldbc_snb_graph, placement_from_schema


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def test_reference_bfs_loads_original_ldbc_csvs(tmp_path: Path) -> None:
    raw = tmp_path / "social_network-sf1-CsvBasic-LongDateFormatter"
    _write(
        raw / "dynamic" / "Person" / "person_0_0.csv",
        "id|firstName|lastName|\n"
        "1|Ada|One|\n"
        "2|Bob|Two|\n"
        "3|Cid|Three|\n",
    )
    _write(
        raw / "dynamic" / "Person_knows_Person" / "person_knows_person_0_0.csv",
        "Person.id|Person.id|creationDate|\n"
        "1|2|2010-01-01T00:00:00.000+0000|\n"
        "2|3|2010-01-01T00:00:00.000+0000|\n",
    )

    schema = tmp_path / "ldbc-snb.graph-shape.json"
    schema.write_text(
        json.dumps(
            {
                "domain": "LDBC SNB Interactive v1",
                "nodeTypes": [
                    {
                        "name": "Person",
                        "source": "Neo4J",
                        "placements": [{"backend": "Neo4J", "count": 3}],
                    }
                ],
                "relations": [
                    {
                        "name": "PERSON_KNOWS_PERSON",
                        "source": "Neo4J",
                        "from": "Person",
                        "to": "Person",
                        "directed": False,
                        "placements": [{"backend": "Neo4J", "count": 2}],
                    }
                ],
            }
        ),
        encoding="utf-8",
    )

    # Reconstruct the same routed IDs independently of graph.jsonl.
    placement = placement_from_schema(schema)
    one, _ = placement.node_route("Person", "1")
    two, _ = placement.node_route("Person", "2")
    three, _ = placement.node_route("Person", "3")

    graph = load_ldbc_snb_graph(raw, schema_path=schema, undirected=True)
    assert graph.bfs_depths(one, max_depth=2) == {one: 0, two: 1, three: 2}


def test_reference_bfs_directed_mode_does_not_add_reverse_edges(tmp_path: Path) -> None:
    raw = tmp_path / "snapshot"
    _write(
        raw / "dynamic" / "Person" / "person_0_0.csv",
        "id|firstName|\n1|A|\n2|B|\n",
    )
    _write(
        raw / "dynamic" / "Person_knows_Person" / "person_knows_person_0_0.csv",
        "Person.id|Person.id|creationDate|\n1|2|x|\n",
    )
    schema = tmp_path / "schema.json"
    schema.write_text(
        json.dumps(
            {
                "nodeTypes": [{"name": "Person", "source": "Neo4J"}],
                "relations": [{"name": "PERSON_KNOWS_PERSON", "source": "Neo4J"}],
            }
        ),
        encoding="utf-8",
    )

    placement = placement_from_schema(schema)
    one, _ = placement.node_route("Person", "1")
    two, _ = placement.node_route("Person", "2")
    graph = load_ldbc_snb_graph(raw, schema_path=schema, undirected=False)

    assert graph.bfs_depths(one, max_depth=1) == {one: 0, two: 1}
    assert graph.bfs_depths(two, max_depth=1) == {two: 0}
