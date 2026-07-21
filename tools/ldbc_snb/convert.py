#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterator

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.graphio import (
    DEFAULT_ROUTING_BACKENDS,
    BackendLayout,
    GraphJsonLinesWriter,
    NodeRecord,
    RelationshipRecord,
    deterministic_entity_uuid,
    deterministic_routed_entity_uuid,
    write_graph_json,
)
from tools.graphio.domain import write_backend_config_for_sources
from tools.ldbc_snb.spec import AUXILIARY_PREFIXES, EDGE_SPECS, NODE_SPECS, EdgeSpec

PARTITION_SUFFIX = re.compile(r"_\d+_\d+$")

NODE_MEANINGS = {
    "Person": "A person participating in the generated social network.",
    "Forum": "A discussion forum containing posts and memberships.",
    "Post": "A top-level message published in a forum.",
    "Comment": "A reply to a post or another comment.",
    "Organisation": "A company or university referenced by person employment and education.",
    "Place": "A geographic place such as a continent, country, or city.",
    "Tag": "A topic tag attached to people, forums, posts, and comments.",
    "TagClass": "A category in the hierarchical tag taxonomy.",
}

RELATION_MEANINGS = {
    "PERSON_KNOWS_PERSON": "Represents an acquaintance connection between two people.",
    "PERSON_LIKES_POST": "Records that a person liked a post.",
    "PERSON_LIKES_COMMENT": "Records that a person liked a comment.",
    "COMMENT_REPLYOF_COMMENT": "Connects a comment to the comment it replies to.",
    "COMMENT_REPLYOF_POST": "Connects a comment to the post it replies to.",
    "FORUM_CONTAINEROF_POST": "Connects a forum to a post contained in it.",
    "FORUM_HASMEMBER_PERSON": "Connects a forum to one of its members.",
    "POST_HASCREATOR_PERSON": "Connects a post to its creator.",
    "COMMENT_HASCREATOR_PERSON": "Connects a comment to its creator.",
}

PROPERTY_MEANINGS = {
    "ldbc_id": "Original numeric identifier from the LDBC dataset.",
    "firstName": "Given name.",
    "lastName": "Family name.",
    "gender": "Generated gender value.",
    "birthday": "Date of birth.",
    "creationDate": "Creation timestamp.",
    "locationIP": "IP address recorded for the generated activity.",
    "browserUsed": "Browser recorded for the generated activity.",
    "language": "Language value or values.",
    "email": "Email address or addresses.",
    "title": "Human-readable title.",
    "content": "Textual message content.",
    "length": "Content length.",
    "name": "Human-readable name.",
    "url": "Generated URL.",
    "type": "LDBC category value.",
    "joinDate": "Timestamp when forum membership began.",
    "classYear": "Graduation or class year.",
    "workFrom": "Year employment began.",
}


@dataclass(frozen=True)
class Placement:
    storage_backends: tuple[str, ...] = DEFAULT_ROUTING_BACKENDS
    default_node_source: str | None = None
    default_relationship_source: str | None = None
    node_sources: dict[str, str] | None = None
    relationship_sources: dict[str, str] | None = None
    _layout: BackendLayout = field(init=False, repr=False, compare=False)

    def __post_init__(self) -> None:
        layout = BackendLayout(tuple(self.storage_backends))
        object.__setattr__(self, "_layout", layout)
        for source in (
            self.default_node_source,
            self.default_relationship_source,
            *(self.node_sources or {}).values(),
            *(self.relationship_sources or {}).values(),
        ):
            if source is not None:
                layout.code_for(source)

    def routing_layout(self) -> BackendLayout:
        return self._layout

    def _route(
        self,
        entity_kind: str,
        type_name: str,
        external_id: str,
        explicit_source: str | None,
    ) -> tuple[str, str]:
        if explicit_source is None:
            return deterministic_routed_entity_uuid(
                entity_kind, type_name, external_id, self._layout
            )
        source = self._layout.source_for_code(self._layout.code_for(explicit_source))
        return (
            deterministic_entity_uuid(
                source, entity_kind, type_name, external_id, self._layout
            ),
            source,
        )

    def node_route(self, type_name: str, external_id: str) -> tuple[str, str]:
        explicit = (self.node_sources or {}).get(type_name, self.default_node_source)
        return self._route("node", type_name, external_id, explicit)

    def relationship_route(self, type_name: str, identity: str) -> tuple[str, str]:
        explicit = (self.relationship_sources or {}).get(
            type_name, self.default_relationship_source
        )
        return self._route("relationship", type_name, identity, explicit)

    @property
    def strategy_name(self) -> str:
        has_explicit = bool(
            self.default_node_source
            or self.default_relationship_source
            or self.node_sources
            or self.relationship_sources
        )
        return "explicit-overrides-v1" if has_explicit else "identity-hash-v1"


def load_placement(
    path: str | None,
    default_source: str,
    storage_backends: tuple[str, ...],
) -> Placement:
    if not path:
        return Placement(storage_backends=storage_backends)
    with Path(path).open("r", encoding="utf-8") as handle:
        raw = json.load(handle)
    configured_backends = tuple(
        str(value)
        for value in raw.get("storageBackends", raw.get("idRouting", storage_backends))
    )
    node_sources = dict(raw.get("nodeSources", raw.get("node_sources", {})))
    relationship_sources = dict(
        raw.get("relationshipSources", raw.get("relationship_sources", {}))
    )
    legacy_fallback = default_source if node_sources or relationship_sources else None
    return Placement(
        storage_backends=configured_backends,
        default_node_source=raw.get(
            "defaultNodeSource", raw.get("default_node_source", legacy_fallback)
        ),
        default_relationship_source=raw.get(
            "defaultRelationshipSource",
            raw.get("default_relationship_source", legacy_fallback),
        ),
        node_sources=node_sources,
        relationship_sources=relationship_sources,
    )


def logical_file_prefix(path: Path) -> str:
    stem = path.name[:-4] if path.name.endswith(".csv") else path.name
    return PARTITION_SUFFIX.sub("", stem)


def discover_csv_files(input_dir: Path) -> dict[str, list[Path]]:
    grouped: dict[str, list[Path]] = defaultdict(list)
    for path in sorted(input_dir.rglob("*.csv")):
        lower = path.name.lower()
        if "updatestream" in lower or "update_stream" in lower:
            continue
        grouped[logical_file_prefix(path)].append(path)
    return grouped


def clean_header(value: str | None, fallback: str) -> str:
    if not value:
        return fallback
    cleaned = value.strip()
    cleaned = re.sub(r":(?:ID|START_ID|END_ID)(?:\([^)]*\))?", "", cleaned, flags=re.IGNORECASE)
    if "." in cleaned:
        cleaned = cleaned.rsplit(".", 1)[-1]
    cleaned = cleaned.strip(":")
    return cleaned or fallback


def parse_scalar(value: str, property_name: str) -> Any:
    text = value.strip()
    if text == "":
        return None
    if property_name in {"id", "ldbc_id", "length", "classYear", "workFrom"}:
        try:
            return int(text)
        except ValueError:
            return text
    if re.fullmatch(r"-?\d+", text) and property_name.lower().endswith("id"):
        try:
            return int(text)
        except ValueError:
            pass
    return text


def csv_rows(path: Path) -> Iterator[tuple[list[str], list[str]]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.reader(handle, delimiter="|")
        try:
            raw_header = next(reader)
        except StopIteration:
            return
        while raw_header and raw_header[-1] == "":
            raw_header.pop()
        header = [clean_header(value, f"field_{index}") for index, value in enumerate(raw_header)]
        for row in reader:
            while row and row[-1] == "" and len(row) > len(header):
                row.pop()
            if not row or all(not value.strip() for value in row):
                continue
            if len(row) < len(header):
                row.extend([""] * (len(header) - len(row)))
            yield header, row[: len(header)]


def read_auxiliary(grouped: dict[str, list[Path]]) -> dict[str, dict[str, list[str]]]:
    result: dict[str, dict[str, list[str]]] = {
        name: defaultdict(list) for name in AUXILIARY_PREFIXES.values()
    }
    for prefix, property_name in AUXILIARY_PREFIXES.items():
        for path in grouped.get(prefix, []):
            for _header, row in csv_rows(path):
                if len(row) >= 2 and row[1].strip():
                    result[property_name][row[0].strip()].append(row[1].strip())
    return result


def row_properties(header: list[str], row: list[str], *, skip: int) -> dict[str, Any]:
    properties: dict[str, Any] = {}
    for index in range(skip, min(len(header), len(row))):
        value = parse_scalar(row[index], header[index])
        if value is not None:
            properties[header[index]] = value
    return properties


def iter_nodes(
    grouped: dict[str, list[Path]],
    placement: Placement,
    auxiliary: dict[str, dict[str, list[str]]],
) -> Iterator[NodeRecord]:
    for prefix, spec in NODE_SPECS.items():
        for path in grouped.get(prefix, []):
            for header, row in csv_rows(path):
                external_id = row[0].strip()
                if not external_id:
                    continue
                node_uuid, source = placement.node_route(spec.type_name, external_id)
                properties = row_properties(header, row, skip=1)
                properties["ldbc_id"] = parse_scalar(external_id, "ldbc_id")
                if spec.type_name == "Person":
                    for property_name in ("email", "language"):
                        values = auxiliary[property_name].get(external_id)
                        if values:
                            properties[property_name] = values
                yield NodeRecord(
                    uuid=node_uuid,
                    type=spec.type_name,
                    source=source,
                    properties=properties,
                )


def edge_identity(spec: EdgeSpec, from_id: str, to_id: str, properties: dict[str, Any]) -> str:
    stable_properties = json.dumps(properties, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return f"{from_id}->{to_id}:{stable_properties}"


def iter_relationships(grouped: dict[str, list[Path]], placement: Placement) -> Iterator[RelationshipRecord]:
    for prefix, spec in EDGE_SPECS.items():
        for path in grouped.get(prefix, []):
            for header, row in csv_rows(path):
                if len(row) < 2:
                    continue
                from_id = row[0].strip()
                to_id = row[1].strip()
                if not from_id or not to_id:
                    continue
                properties = row_properties(header, row, skip=2)
                identity = edge_identity(spec, from_id, to_id, properties)
                relationship_uuid, source = placement.relationship_route(spec.type_name, identity)
                from_uuid, _ = placement.node_route(spec.from_type, from_id)
                to_uuid, _ = placement.node_route(spec.to_type, to_id)
                yield RelationshipRecord(
                    uuid=relationship_uuid,
                    type=spec.type_name,
                    source=source,
                    from_uuid=from_uuid,
                    to_uuid=to_uuid,
                    properties=properties,
                )


def _default_sample(type_name: str) -> Any:
    return {
        "integer": 1,
        "float": 1.0,
        "boolean": True,
        "date": "2010-01-01",
        "datetime": "2010-01-01T00:00:00.000+0000",
    }.get(type_name, "example")


def _property_json(name: str, type_name: str, samples: dict[str, Any]) -> dict[str, Any]:
    return {
        "name": name,
        "type": type_name,
        "meaning": PROPERTY_MEANINGS.get(name, f"LDBC property '{name}'."),
        "sampleValue": samples.get(name, _default_sample(type_name)),
    }


def collect_samples(
    grouped: dict[str, list[Path]],
    placement: Placement,
    auxiliary: dict[str, dict[str, list[str]]],
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    node_samples: dict[str, dict[str, Any]] = {}
    relationship_samples: dict[str, dict[str, Any]] = {}
    for node in iter_nodes(grouped, placement, auxiliary):
        node_samples.setdefault(node.type, node.properties)
        if len(node_samples) == len(NODE_SPECS):
            break
    for relationship in iter_relationships(grouped, placement):
        relationship_samples.setdefault(relationship.type, relationship.properties)
        if len(relationship_samples) == len(EDGE_SPECS):
            break
    return node_samples, relationship_samples


def write_schema(
    path: Path,
    placement: Placement,
    *,
    node_samples: dict[str, dict[str, Any]] | None = None,
    relationship_samples: dict[str, dict[str, Any]] | None = None,
) -> None:
    node_samples = node_samples or {}
    relationship_samples = relationship_samples or {}
    connects_to: dict[str, set[str]] = {spec.type_name: set() for spec in NODE_SPECS.values()}
    for relation in EDGE_SPECS.values():
        connects_to[relation.from_type].add(relation.to_type)
        connects_to[relation.to_type].add(relation.from_type)

    schema = {
        "domain": "LDBC SNB Interactive v1",
        "description": (
            "The LDBC Social Network Benchmark Interactive v1 graph, including both the static "
            "reference data and the dynamic initial social-network snapshot. Update streams are not included."
        ),
        "nodeTypes": [
            {
                "name": spec.type_name,
                "meaning": NODE_MEANINGS.get(spec.type_name, f"LDBC {spec.type_name} entity."),
                "connectsTo": sorted(connects_to[spec.type_name]),
                "properties": [
                    _property_json(name, type_name, node_samples.get(spec.type_name, {}))
                    for name, type_name in spec.properties
                ],
            }
            for spec in NODE_SPECS.values()
        ],
        "relations": [
            {
                "name": spec.type_name,
                "meaning": RELATION_MEANINGS.get(
                    spec.type_name,
                    f"Connects {spec.from_type} to {spec.to_type} in the LDBC SNB graph.",
                ),
                "from": spec.from_type,
                "to": spec.to_type,
                "directed": spec.directed,
                "properties": [
                    _property_json(name, type_name, relationship_samples.get(spec.type_name, {}))
                    for name, type_name in spec.properties
                ],
            }
            for spec in EDGE_SPECS.values()
        ],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(schema, indent=2) + "\n", encoding="utf-8")


def write_jsonl(
    output: Path,
    grouped: dict[str, list[Path]],
    placement: Placement,
    auxiliary: dict[str, dict[str, list[str]]],
) -> tuple[int, int]:
    node_count = relationship_count = 0
    routing_layout = placement.routing_layout()
    with GraphJsonLinesWriter(
        output,
        {
            "dataset": "ldbc-snb-interactive-v1",
            "idRoutingStrategy": placement.strategy_name,
            **routing_layout.metadata(),
        },
    ) as writer:
        for node in iter_nodes(grouped, placement, auxiliary):
            writer.write_node(node)
            node_count += 1
        for relationship in iter_relationships(grouped, placement):
            writer.write_relationship(relationship)
            relationship_count += 1
    return node_count, relationship_count


def write_monolithic_json(
    output: Path,
    grouped: dict[str, list[Path]],
    placement: Placement,
    auxiliary: dict[str, dict[str, list[str]]],
) -> tuple[int, int]:
    nodes = list(iter_nodes(grouped, placement, auxiliary))
    relationships = list(iter_relationships(grouped, placement))
    write_graph_json(
        output,
        nodes,
        relationships,
        metadata={
            "idRoutingStrategy": placement.strategy_name,
            **placement.routing_layout().metadata(),
        },
    )
    return len(nodes), len(relationships)


def convert_dataset(
    *,
    input_dir: str | Path,
    output: str | Path,
    schema_output: str | Path,
    backend_config_output: str | Path | None = None,
    placement_config: str | None = None,
    default_source: str = "Neo4J",
    storage_backends: tuple[str, ...] | list[str] = DEFAULT_ROUTING_BACKENDS,
    arango_database: str = "university",
) -> dict[str, Any]:
    source_dir = Path(input_dir).expanduser().resolve()
    graph_output = Path(output).expanduser().resolve()
    schema_path = Path(schema_output).expanduser().resolve()
    if not source_dir.exists():
        raise FileNotFoundError(f"Input directory does not exist: {source_dir}")

    grouped = discover_csv_files(source_dir)
    missing_nodes = [prefix for prefix in NODE_SPECS if prefix not in grouped]
    if missing_nodes:
        print(f"warning: missing SNB node files: {', '.join(missing_nodes)}", file=sys.stderr)
    unknown = sorted(set(grouped) - set(NODE_SPECS) - set(EDGE_SPECS) - set(AUXILIARY_PREFIXES))
    if unknown:
        print(f"warning: ignored unrecognized CSV groups: {', '.join(unknown)}", file=sys.stderr)

    placement = load_placement(placement_config, default_source, tuple(storage_backends))
    auxiliary = read_auxiliary(grouped)
    graph_output.parent.mkdir(parents=True, exist_ok=True)
    if graph_output.suffix.lower() in {".jsonl", ".ndjson"}:
        node_count, relationship_count = write_jsonl(graph_output, grouped, placement, auxiliary)
    elif graph_output.suffix.lower() == ".json":
        node_count, relationship_count = write_monolithic_json(graph_output, grouped, placement, auxiliary)
    else:
        raise ValueError("Output must end in .json, .jsonl, or .ndjson")

    node_samples, relationship_samples = collect_samples(grouped, placement, auxiliary)
    write_schema(
        schema_path,
        placement,
        node_samples=node_samples,
        relationship_samples=relationship_samples,
    )
    if backend_config_output:
        write_backend_config_for_sources(
            placement.routing_layout().backends,
            backend_config_output,
            arango_database=arango_database,
        )

    return {
        "input": str(source_dir),
        "output": str(graph_output),
        "schema": str(schema_path),
        "nodes": node_count,
        "relationships": relationship_count,
        "uuid_routing": list(placement.routing_layout().backends),
        "uuid_routing_bits": placement.routing_layout().bit_count,
        "ignored_csv_groups": unknown,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert LDBC SNB Interactive v1 CsvBasic static+dynamic data to CPGDB."
    )
    parser.add_argument("--input-dir", required=True, help="Directory containing sibling static/ and dynamic/ folders.")
    parser.add_argument("--output", required=True, help="Output .jsonl/.ndjson (recommended) or .json graph file.")
    parser.add_argument("--schema-output", required=True, help="Generated graph-shape schema.")
    parser.add_argument("--backend-config-output", help="Generated backend configuration matching placement.")
    parser.add_argument(
        "--placement-config",
        help="Optional legacy type overrides; omitted means deterministic per-object ID routing.",
    )
    parser.add_argument(
        "--storage-backends",
        nargs="+",
        choices=["Neo4J", "ArangoDB", "GPU"],
        default=list(DEFAULT_ROUTING_BACKENDS),
        help="Ordered backend list; B=3 automatically uses two ID MSB routing bits.",
    )
    parser.add_argument("--default-source", default="Neo4J", choices=["Neo4J", "ArangoDB", "GPU"])
    parser.add_argument("--arango-database", default="university")
    args = parser.parse_args()

    result = convert_dataset(
        input_dir=args.input_dir,
        output=args.output,
        schema_output=args.schema_output,
        backend_config_output=args.backend_config_output,
        placement_config=args.placement_config,
        default_source=args.default_source,
        storage_backends=args.storage_backends,
        arango_database=args.arango_database,
    )
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
