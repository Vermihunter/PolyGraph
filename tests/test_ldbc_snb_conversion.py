from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.graphio import open_graph_reader
from tools.graphio.id_codec import backend_id, decode_backend_id


def test_ldbc_converter_jsonl(tmp_path: Path) -> None:
    output = tmp_path / "graph.jsonl"
    schema = tmp_path / "schema.json"
    backends = tmp_path / "backends.json"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools/ldbc_snb/convert.py"),
            "--input-dir", str(ROOT / "tests/fixtures/ldbc_snb/social_network"),
            "--output", str(output),
            "--schema-output", str(schema),
            "--backend-config-output", str(backends),
        ],
        check=True,
    )

    graph = open_graph_reader(output).load()
    assert len(graph.nodes) == 7
    assert len(graph.relationships) == 7
    assert all(decode_backend_id(node.uuid) == backend_id(node.source) for node in graph.nodes)
    assert all(decode_backend_id(edge.uuid) == backend_id(edge.source) for edge in graph.relationships)

    alice = next(node for node in graph.nodes if node.type == "Person" and node.properties["ldbc_id"] == 1)
    assert alice.properties["email"] == ["alice@example.com"]
    assert alice.properties["language"] == ["en"]

    schema_json = json.loads(schema.read_text())
    assert any(node["name"] == "Person" for node in schema_json["nodeTypes"])
    assert any(rel["name"] == "PERSON_KNOWS_PERSON" for rel in schema_json["relations"])
    assert any(rel["name"] == "COMMENT_HASTAG_TAG" for rel in schema_json["relations"])

    membership = next(edge for edge in graph.relationships if edge.type == "FORUM_HASMEMBER_PERSON")
    assert membership.properties["joinDate"] == "2012-01-04T00:00:00.000+0000"


def test_uuid_generation_is_reproducible(tmp_path: Path) -> None:
    outputs = [tmp_path / "a.jsonl", tmp_path / "b.jsonl"]
    for output in outputs:
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/ldbc_snb/convert.py"),
                "--input-dir", str(ROOT / "tests/fixtures/ldbc_snb/social_network"),
                "--output", str(output),
                "--schema-output", str(output.with_suffix(".schema.json")),
            ],
            check=True,
        )
    assert outputs[0].read_bytes() == outputs[1].read_bytes()


def test_large_hadoop_person_ids_can_use_multiple_upper_namespaces() -> None:
    from tools.ldbc_snb.convert import NODE_LOCAL_ID_MASK, Placement

    placement = Placement(storage_backends=("Neo4J",))
    ids = [23057, (1 << 45) + 23057, (2 << 45) + 23057]
    uuids = [placement.node_route("Person", str(raw_id))[0] for raw_id in ids]

    assert len(set(uuids)) == len(ids)
    assert placement.node_prefix_count == 3
    assert all((raw_id & NODE_LOCAL_ID_MASK) == 23057 for raw_id in ids)


def test_same_raw_ldbc_id_is_separated_by_node_type() -> None:
    from tools.ldbc_snb.convert import Placement

    placement = Placement(storage_backends=("Neo4J",))
    raw_id = (1 << 45) + 7
    person_uuid, _ = placement.node_route("Person", str(raw_id))
    forum_uuid, _ = placement.node_route("Forum", str(raw_id))

    assert person_uuid != forum_uuid


def test_same_low32_component_is_separated_by_raw_prefix() -> None:
    from tools.ldbc_snb.convert import Placement

    placement = Placement(storage_backends=("Neo4J",))
    low = 123456
    first_uuid, _ = placement.node_route("Person", str(low))
    second_uuid, _ = placement.node_route("Person", str((1 << 45) + low))

    assert first_uuid != second_uuid


def test_node_prefix_encoding_is_reproducible_for_stable_order() -> None:
    from tools.ldbc_snb.convert import Placement

    values = [
        ("Person", "1"),
        ("Person", str((1 << 45) + 23057)),
        ("Forum", "10"),
        ("Post", str((3 << 45) + 11)),
    ]
    outputs = []
    for _ in range(2):
        placement = Placement(storage_backends=("Neo4J",))
        outputs.append([placement.node_route(type_name, raw_id)[0] for type_name, raw_id in values])

    assert outputs[0] == outputs[1]



def test_node_and_relationship_payload_namespaces_do_not_overlap() -> None:
    from tools.ldbc_snb.convert import ENTITY_KIND_BIT, Placement

    placement = Placement(
        storage_backends=("Neo4J",),
        default_node_source="Neo4J",
        default_relationship_source="Neo4J",
    )
    node_uuid, _ = placement.node_route("Person", "0")
    relationship_uuid = placement.relationship_uuid("COMMENT_HASCREATOR_PERSON", "Neo4J", 0)
    layout = placement.routing_layout()

    assert layout.local_id(node_uuid) & ENTITY_KIND_BIT == 0
    assert layout.local_id(relationship_uuid) & ENTITY_KIND_BIT != 0
    assert node_uuid != relationship_uuid


def test_attribute_hash_placement_is_deterministic_and_uses_all_backends() -> None:
    from tools.ldbc_snb.convert import Placement

    placement = Placement(
        storage_backends=("Neo4J", "ArangoDB", "PostgreSQL"),
        strategy="attribute-hash",
        node_hash_attributes={"*": ("$type", "ldbc_id")},
        relationship_hash_attributes={"*": ("$type", "$from", "$to")},
    )

    first = [
        placement.node_route("Person", str(value), {"ldbc_id": value})[1]
        for value in range(300)
    ]
    second = [
        placement.node_route("Person", str(value), {"ldbc_id": value})[1]
        for value in range(300)
    ]

    assert first == second
    assert set(first) == {"Neo4J", "ArangoDB", "PostgreSQL"}


def test_label_striped_placement_distributes_each_label_over_all_backends() -> None:
    from tools.ldbc_snb.convert import Placement

    placement = Placement(
        storage_backends=("Neo4J", "ArangoDB", "PostgreSQL"),
        strategy="label-striped",
    )

    for type_name in ("Person", "Post", "Tag"):
        sources = {
            placement.node_route(type_name, str(value), {"ldbc_id": value})[1]
            for value in range(300)
        }
        assert sources == {"Neo4J", "ArangoDB", "PostgreSQL"}


def test_hash_and_label_striped_configs_preserve_routing_consistency(tmp_path: Path) -> None:
    from tools.ldbc_snb.convert import convert_dataset

    fixture = ROOT / "tests/fixtures/ldbc_snb/social_network"
    configs = (
        ROOT / "tools/ldbc_snb/placement.hash-attributes-3db.json",
        ROOT / "tools/ldbc_snb/placement.label-striped-3db.json",
    )

    for config in configs:
        output = tmp_path / f"{config.stem}.jsonl"
        convert_dataset(
            input_dir=fixture,
            output=output,
            schema_output=output.with_suffix(".schema.json"),
            placement_config=str(config),
            storage_backends=("Neo4J", "ArangoDB", "PostgreSQL"),
            render_placement_graph=False,
        )
        graph = open_graph_reader(output).load()
        assert all(decode_backend_id(node.uuid) == backend_id(node.source) for node in graph.nodes)
        assert all(decode_backend_id(edge.uuid) == backend_id(edge.source) for edge in graph.relationships)


def test_proxy_optimizer_uses_all_three_backends_on_fixture(tmp_path: Path) -> None:
    placement = tmp_path / "placement.json"
    report = tmp_path / "report.json"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools/ldbc_snb/optimize_proxy_placement.py"),
            "--input", str(ROOT / "tests/fixtures/ldbc_snb/social_network"),
            "--output", str(placement),
            "--report", str(report),
        ],
        check=True,
    )
    data = json.loads(report.read_text(encoding="utf-8"))
    assert set(data["nodeSources"].values()) == {"Neo4J", "ArangoDB", "PostgreSQL"}
    assert all(count > 0 for count in data["backendNodeCounts"].values())
    assert data["estimatedProxyCopies"] >= 0
