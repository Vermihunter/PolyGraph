from __future__ import annotations

import json
from pathlib import Path

from tools.ldbc_snb.convert import convert_dataset


def _write_csv(path: Path, header: str, *rows: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join((header, *rows)) + "\n", encoding="utf-8")


def test_convert_writes_backend_container_report_without_second_graph_pass(tmp_path: Path) -> None:
    input_dir = tmp_path / "social_network"
    dynamic = input_dir / "dynamic"

    _write_csv(
        dynamic / "person_0_0.csv",
        "id:ID(Person)|firstName|",
        "1|Alice|",
        "2|Bob|",
    )
    _write_csv(dynamic / "post_0_0.csv", "id:ID(Post)|content|", "10|Post|")
    _write_csv(dynamic / "comment_0_0.csv", "id:ID(Comment)|content|", "20|Comment|")
    _write_csv(dynamic / "tag_0_0.csv", "id:ID(Tag)|name|", "30|Graph|")
    _write_csv(dynamic / "tagclass_0_0.csv", "id:ID(TagClass)|name|", "40|Topic|")
    _write_csv(
        dynamic / "post_hasCreator_person_0_0.csv",
        "Post.id:START_ID(Post)|Person.id:END_ID(Person)|",
        "10|1|",
    )
    _write_csv(
        dynamic / "comment_hasCreator_person_0_0.csv",
        "Comment.id:START_ID(Comment)|Person.id:END_ID(Person)|",
        "20|2|",
    )
    _write_csv(
        dynamic / "tag_hasType_tagclass_0_0.csv",
        "Tag.id:START_ID(Tag)|TagClass.id:END_ID(TagClass)|",
        "30|40|",
    )

    placement = tmp_path / "placement.config.json"
    placement.write_text(
        json.dumps(
            {
                "storageBackends": ["Neo4J", "ArangoDB", "GPU"],
                "defaultNodeSource": "Neo4J",
                "defaultRelationshipSource": "Neo4J",
                "nodeSources": {
                    "Post": "ArangoDB",
                    "Comment": "ArangoDB",
                    "Tag": "GPU",
                    "TagClass": "GPU",
                },
                "relationshipSources": {
                    "POST_HASCREATOR_PERSON": "ArangoDB",
                    "COMMENT_HASCREATOR_PERSON": "ArangoDB",
                    "TAG_HASTYPE_TAGCLASS": "GPU",
                },
            }
        ),
        encoding="utf-8",
    )

    output_dir = tmp_path / "prepared"
    result = convert_dataset(
        input_dir=input_dir,
        output=output_dir / "graph.jsonl",
        schema_output=output_dir / "schema.json",
        placement_config=str(placement),
        placement_report_dir=output_dir,
        render_placement_graph=False,
    )

    assert result["nodes"] == 6
    assert result["relationships"] == 3
    assert set(result["placement"]) == {"json", "text", "dot"}

    report = json.loads((output_dir / "placement-report.json").read_text())
    assert report["summary"]["nodeCount"] == 6
    assert report["summary"]["relationshipCount"] == 3
    assert report["backends"]["Neo4J"]["nodeTypes"] == {"Person": 2}
    assert report["backends"]["ArangoDB"]["nodeTypes"] == {
        "Comment": 1,
        "Post": 1,
    }
    assert report["backends"]["GPU"]["nodeTypes"] == {
        "Tag": 1,
        "TagClass": 1,
    }
    assert report["backends"]["ArangoDB"]["relationshipTypes"] == {
        "COMMENT_HASCREATOR_PERSON": 1,
        "POST_HASCREATOR_PERSON": 1,
    }
    assert report["backends"]["GPU"]["relationshipTypes"] == {
        "TAG_HASTYPE_TAGCLASS": 1
    }
    assert report["summary"]["relationshipsWithRemoteEndpoint"] == 2

    dot = (output_dir / "placement-graph.dot").read_text()
    assert "subgraph cluster_0" in dot
    assert "Neo4J\\\\nNodes: 2 | Relationships: 0" in dot
    assert "ArangoDB\\\\nNodes: 2 | Relationships: 2" in dot
    assert "GPU\\\\nNodes: 2 | Relationships: 1" in dot
    assert "POST_HASCREATOR_PERSON" in dot
    assert 'style="dashed"' in dot

    text = (output_dir / "placement-report.txt").read_text()
    assert "Observed backend connections" in text
    assert "Post [ArangoDB] -- POST_HASCREATOR_PERSON [ArangoDB] --> Person [Neo4J]" in text

    schema = json.loads((output_dir / "schema.json").read_text())
    node_types = {item["name"]: item for item in schema["nodeTypes"]}
    relations = {item["name"]: item for item in schema["relations"]}
    assert node_types["Post"]["source"] == "ArangoDB"
    assert node_types["Post"]["count"] == 1
    assert node_types["Post"]["placements"] == [
        {"backend": "ArangoDB", "count": 1}
    ]
    assert relations["TAG_HASTYPE_TAGCLASS"]["source"] == "GPU"
    assert relations["TAG_HASTYPE_TAGCLASS"]["count"] == 1
