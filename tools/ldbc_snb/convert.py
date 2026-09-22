#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import time
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
    plan_graph_caches,
    load_cache_config,
    plan_gpu_storage,
    load_gpu_distribution_config,
)
from tools.graphio.domain import write_backend_config_for_sources
from tools.ldbc_snb.placement_report import (
    PlacementStatistics,
    build_placement_report,
    write_placement_outputs,
)
from tools.ldbc_snb.spec import (
    AUXILIARY_PREFIXES,
    EDGE_SPECS,
    NODE_SPECS,
    EdgeSpec,
    NodeSpec,
)

PARTITION_SUFFIX = re.compile(r"_\d+_\d+$")
PARTITION_KEY = re.compile(r"(_\d+_\d+)(?:\.csv)?$", re.IGNORECASE)

# The global ID format reserves 48 payload bits below the 16 location bits.
#
# LDBC Hadoop Datagen IDs are large sparse 64-bit-ish integers whose upper bits
# are part of Datagen's own allocation scheme.  They are not a single namespace
# per entity type, so neither reserving fixed type bits nor masking the upper
# Datagen bits is safe.
#
# The highest payload bit separates nodes from relationships, preserving the
# original converter's guarantee that all entity IDs owned by the same backend
# share one collision-free namespace.
#
# For nodes, preserve the low 31 raw-ID bits exactly and compact only the pair
#   (PolyGraph node type, raw LDBC high prefix)
# into 16 prefix bits.  Therefore node payloads use 47 bits in total and always
# have payload bit 47 clear.  The dictionary is bounded to 2^16 entries, so RAM
# depends on the number of ID blocks, not on |V|.
ENTITY_KIND_BIT = 1 << 47
NODE_LOCAL_ID_BITS = 31
NODE_PREFIX_BITS = 16
NODE_LOCAL_ID_MASK = (1 << NODE_LOCAL_ID_BITS) - 1
NODE_PREFIX_MASK = (1 << NODE_PREFIX_BITS) - 1
NODE_TYPE_NAMES = frozenset(spec.type_name for spec in NODE_SPECS.values())

# Relationships live in the upper half of the payload space (kind bit = 1) and
# use deterministic per-(type, backend) sequence IDs.  With 23 LDBC relation
# types this leaves 42 sequence bits per type/backend.
RELATIONSHIP_TYPE_BITS = max(1, (len(EDGE_SPECS) - 1).bit_length())
RELATIONSHIP_SEQUENCE_BITS = 47 - RELATIONSHIP_TYPE_BITS
RELATIONSHIP_SEQUENCE_MASK = (1 << RELATIONSHIP_SEQUENCE_BITS) - 1
RELATIONSHIP_TYPE_CODES = {
    spec.type_name: code for code, spec in enumerate(EDGE_SPECS.values())
}

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
    _node_prefix_codes: dict[tuple[str, int], int] = field(init=False, repr=False, compare=False)

    def __post_init__(self) -> None:
        layout = BackendLayout(tuple(self.storage_backends))
        object.__setattr__(self, "_layout", layout)
        object.__setattr__(self, "_node_prefix_codes", {})
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

    def node_route(self, type_name: str, external_id: str) -> tuple[int, str]:
        explicit = (self.node_sources or {}).get(type_name, self.default_node_source)
        identity = f"node:{type_name}:{external_id}"
        source = (
            self._layout.source_for_identity(identity)
            if explicit is None
            else self._layout.source_for_code(self._layout.code_for(explicit))
        )
        try:
            numeric_id = int(external_id)
        except ValueError as exc:
            raise ValueError(
                f"LDBC node ID must be numeric for scalable direct ID encoding: "
                f"type={type_name} id={external_id!r}"
            ) from exc
        if numeric_id < 0:
            raise ValueError(
                f"LDBC node ID must be non-negative: type={type_name} id={numeric_id}"
            )
        if type_name not in NODE_TYPE_NAMES:
            raise ValueError(f"Unknown LDBC node type for ID encoding: {type_name}")

        raw_prefix = numeric_id >> NODE_LOCAL_ID_BITS
        prefix_key = (type_name, raw_prefix)
        prefix_code = self._node_prefix_codes.get(prefix_key)
        if prefix_code is None:
            prefix_code = len(self._node_prefix_codes)
            if prefix_code > NODE_PREFIX_MASK:
                raise OverflowError(
                    "LDBC node IDs require more than "
                    f"{NODE_PREFIX_MASK + 1:,} distinct (type, high-32) prefixes; "
                    "the bounded 48-bit PolyGraph payload prefix table is exhausted"
                )
            self._node_prefix_codes[prefix_key] = prefix_code

        local_component = numeric_id & NODE_LOCAL_ID_MASK
        payload = (prefix_code << NODE_LOCAL_ID_BITS) | local_component
        return self._layout.encode_local(payload, source), source

    def relationship_source(self, type_name: str, identity: str | None = None) -> str:
        explicit = (self.relationship_sources or {}).get(
            type_name, self.default_relationship_source
        )
        if explicit is not None:
            return self._layout.source_for_code(self._layout.code_for(explicit))
        if identity is None:
            raise ValueError(
                f"Relationship identity is required for hash placement of {type_name}"
            )
        return self._layout.source_for_identity(f"relationship:{type_name}:{identity}")

    def relationship_uuid(self, type_name: str, source: str, sequence: int) -> int:
        if sequence < 0 or sequence > RELATIONSHIP_SEQUENCE_MASK:
            raise OverflowError(
                f"LDBC relationship sequence {sequence} for {type_name}/{source} "
                f"does not fit the {RELATIONSHIP_SEQUENCE_BITS}-bit namespace"
            )
        try:
            type_code = RELATIONSHIP_TYPE_CODES[type_name]
        except KeyError as exc:
            raise ValueError(
                f"Unknown LDBC relationship type for ID encoding: {type_name}"
            ) from exc
        payload = ENTITY_KIND_BIT | (type_code << RELATIONSHIP_SEQUENCE_BITS) | sequence
        return self._layout.encode_local(payload, source)

    def configured_node_source(self, type_name: str) -> str | None:
        source = (self.node_sources or {}).get(type_name, self.default_node_source)
        if source is None:
            return None
        return self._layout.source_for_code(self._layout.code_for(source))

    def configured_relationship_source(self, type_name: str) -> str | None:
        source = (self.relationship_sources or {}).get(
            type_name, self.default_relationship_source
        )
        if source is None:
            return None
        return self._layout.source_for_code(self._layout.code_for(source))


    @property
    def node_prefix_count(self) -> int:
        return len(self._node_prefix_codes)

    @property
    def strategy_name(self) -> str:
        has_explicit = bool(
            self.default_node_source
            or self.default_relationship_source
            or self.node_sources
            or self.relationship_sources
        )
        return "explicit-overrides-v1" if has_explicit else "identity-hash-v1"

    @property
    def payload_strategy_name(self) -> str:
        return "ldbc-block31-v3"


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


def partition_key(path: Path) -> str | None:
    match = PARTITION_KEY.search(path.name)
    return match.group(1) if match else None


def _auxiliary_paths_for_person_partition(
    grouped: dict[str, list[Path]],
    prefix: str,
    person_path: Path,
) -> list[Path]:
    candidates = grouped.get(prefix, [])
    if not candidates:
        return []
    key = partition_key(person_path)
    if key is not None:
        exact = [path for path in candidates if partition_key(path) == key]
        if exact:
            return exact
    # Unpartitioned exports commonly contain a single auxiliary file.  Keep a
    # safe fallback for those while avoiding a global SF-scale in-memory map.
    if len(candidates) == 1:
        return candidates
    if key is None:
        return candidates
    return []


def read_person_auxiliary_partition(
    grouped: dict[str, list[Path]], person_path: Path
) -> dict[str, dict[str, list[str]]]:
    """Load only the email/language shard matching one Person CSV shard.

    Old conversion loaded every Person auxiliary value into Python at once.
    At large scale factors that alone can consume multiple GiB.  LDBC CsvBasic
    uses matching partition suffixes, so processing one shard at a time bounds
    auxiliary memory by the largest shard rather than the whole dataset.
    """

    result: dict[str, dict[str, list[str]]] = {
        name: defaultdict(list) for name in AUXILIARY_PREFIXES.values()
    }
    for prefix, property_name in AUXILIARY_PREFIXES.items():
        for path in _auxiliary_paths_for_person_partition(grouped, prefix, person_path):
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
) -> Iterator[NodeRecord]:
    for prefix, spec in NODE_SPECS.items():
        for path in grouped.get(prefix, []):
            auxiliary = (
                read_person_auxiliary_partition(grouped, path)
                if spec.type_name == "Person"
                else None
            )
            for header, row in csv_rows(path):
                external_id = row[0].strip()
                if not external_id:
                    continue
                node_uuid, source = placement.node_route(spec.type_name, external_id)
                properties = row_properties(header, row, skip=1)
                properties["ldbc_id"] = parse_scalar(external_id, "ldbc_id")
                if auxiliary is not None:
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


def edge_identity(from_id: str, to_id: str, raw_property_values: list[str]) -> str:
    # Used only for identity-hash *placement*.  Relationship IDs themselves are
    # allocated from deterministic counters, so schema-based placement avoids
    # this string construction entirely.
    return f"{from_id}->{to_id}:" + "|".join(raw_property_values)


def iter_relationships(grouped: dict[str, list[Path]], placement: Placement) -> Iterator[RelationshipRecord]:
    sequence_by_type_and_source: dict[tuple[str, str], int] = defaultdict(int)
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
                configured_source = placement.configured_relationship_source(spec.type_name)
                identity = None if configured_source is not None else edge_identity(from_id, to_id, row[2:])
                source = placement.relationship_source(spec.type_name, identity)
                sequence_key = (spec.type_name, source)
                sequence = sequence_by_type_and_source[sequence_key]
                sequence_by_type_and_source[sequence_key] += 1
                relationship_uuid = placement.relationship_uuid(spec.type_name, source, sequence)
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
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    node_samples: dict[str, dict[str, Any]] = {}
    relationship_samples: dict[str, dict[str, Any]] = {}
    for node in iter_nodes(grouped, placement):
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
    placement_statistics: PlacementStatistics | None = None,
) -> None:
    node_samples = node_samples or {}
    relationship_samples = relationship_samples or {}
    connects_to: dict[str, set[str]] = {spec.type_name: set() for spec in NODE_SPECS.values()}
    for relation in EDGE_SPECS.values():
        connects_to[relation.from_type].add(relation.to_type)
        connects_to[relation.to_type].add(relation.from_type)

    def placement_fields(
        counts: dict[str, int], configured_source: str | None
    ) -> dict[str, Any]:
        placements = [
            {"backend": backend, "count": count}
            for backend, count in sorted(counts.items())
        ]
        if configured_source is not None:
            source = configured_source
        elif len(counts) == 1:
            source = next(iter(counts))
        else:
            source = "Distributed"
        return {
            "source": source,
            "count": sum(counts.values()),
            "placements": placements,
        }

    def node_schema(spec: NodeSpec) -> dict[str, Any]:
        counts = (
            placement_statistics.node_type_counts(spec.type_name)
            if placement_statistics
            else {}
        )
        return {
            "name": spec.type_name,
            "meaning": NODE_MEANINGS.get(
                spec.type_name, f"LDBC {spec.type_name} entity."
            ),
            "connectsTo": sorted(connects_to[spec.type_name]),
            **placement_fields(
                counts, placement.configured_node_source(spec.type_name)
            ),
            "properties": [
                _property_json(
                    name, type_name, node_samples.get(spec.type_name, {})
                )
                for name, type_name in spec.properties
            ],
        }

    def relationship_schema(spec: EdgeSpec) -> dict[str, Any]:
        counts = (
            placement_statistics.relationship_type_counts(spec.type_name)
            if placement_statistics
            else {}
        )
        return {
            "name": spec.type_name,
            "meaning": RELATION_MEANINGS.get(
                spec.type_name,
                f"Connects {spec.from_type} to {spec.to_type} in the LDBC SNB graph.",
            ),
            "from": spec.from_type,
            "to": spec.to_type,
            "directed": spec.directed,
            **placement_fields(
                counts, placement.configured_relationship_source(spec.type_name)
            ),
            "properties": [
                _property_json(
                    name,
                    type_name,
                    relationship_samples.get(spec.type_name, {}),
                )
                for name, type_name in spec.properties
            ],
        }

    schema = {
        "domain": "LDBC SNB Interactive v1",
        "description": (
            "The LDBC Social Network Benchmark Interactive v1 graph, including both the static "
            "reference data and the dynamic initial social-network snapshot. Update streams are not included."
        ),
        "nodeTypes": [node_schema(spec) for spec in NODE_SPECS.values()],
        "relations": [relationship_schema(spec) for spec in EDGE_SPECS.values()],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(schema, indent=2) + "\n", encoding="utf-8")


def write_jsonl(
    output: Path,
    grouped: dict[str, list[Path]],
    placement: Placement,
    placement_statistics: PlacementStatistics | None = None,
) -> tuple[int, int]:
    node_count = relationship_count = 0
    routing_layout = placement.routing_layout()
    with GraphJsonLinesWriter(
        output,
        {
            "dataset": "ldbc-snb-interactive-v1",
            "idRoutingStrategy": placement.strategy_name,
            "idPayloadStrategy": placement.payload_strategy_name,
            **routing_layout.metadata(),
        },
    ) as writer:
        started = time.monotonic()
        last_report = started
        for node in iter_nodes(grouped, placement):
            writer.write_node(node)
            if placement_statistics is not None:
                placement_statistics.record_node(node)
            node_count += 1
            now = time.monotonic()
            if now - last_report >= 5.0:
                print(
                    f"[ldbc] wrote {node_count:,} nodes ({now - started:,.1f}s)",
                    file=sys.stderr,
                    flush=True,
                )
                last_report = now
        print(
            f"[ldbc] node ID prefix table: {placement.node_prefix_count:,}/"
            f"{NODE_PREFIX_MASK + 1:,} entries",
            file=sys.stderr,
            flush=True,
        )
        for relationship in iter_relationships(grouped, placement):
            writer.write_relationship(relationship)
            if placement_statistics is not None:
                placement_statistics.record_relationship(relationship, routing_layout)
            relationship_count += 1
            now = time.monotonic()
            if now - last_report >= 5.0:
                print(
                    f"[ldbc] wrote {node_count:,} nodes and {relationship_count:,} relationships "
                    f"({now - started:,.1f}s)",
                    file=sys.stderr,
                    flush=True,
                )
                last_report = now
    return node_count, relationship_count


def write_monolithic_json(
    output: Path,
    grouped: dict[str, list[Path]],
    placement: Placement,
    placement_statistics: PlacementStatistics | None = None,
) -> tuple[int, int]:
    nodes = list(iter_nodes(grouped, placement))
    relationships = list(iter_relationships(grouped, placement))
    if placement_statistics is not None:
        routing_layout = placement.routing_layout()
        for node in nodes:
            placement_statistics.record_node(node)
        for relationship in relationships:
            placement_statistics.record_relationship(relationship, routing_layout)
    write_graph_json(
        output,
        nodes,
        relationships,
        metadata={
            "idRoutingStrategy": placement.strategy_name,
            "idPayloadStrategy": placement.payload_strategy_name,
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
    placement_report_dir: str | Path | None = None,
    render_placement_graph: bool = True,
    cache_config: str | Path | None = None,
    cache_output_dir: str | Path | None = None,
    gpu_distribution_config: str | Path | None = None,
    gpu_distribution_output_dir: str | Path | None = None,
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
    placement_statistics = PlacementStatistics()
    graph_output.parent.mkdir(parents=True, exist_ok=True)
    print(
        f"[ldbc] discovered {sum(len(paths) for paths in grouped.values()):,} CSV shards; "
        f"streaming conversion with bounded-memory ID allocation",
        file=sys.stderr,
        flush=True,
    )
    if graph_output.suffix.lower() in {".jsonl", ".ndjson"}:
        node_count, relationship_count = write_jsonl(
            graph_output,
            grouped,
            placement,
            placement_statistics,
        )
    elif graph_output.suffix.lower() == ".json":
        node_count, relationship_count = write_monolithic_json(
            graph_output,
            grouped,
            placement,
            placement_statistics,
        )
    else:
        raise ValueError("Output must end in .json, .jsonl, or .ndjson")

    node_samples, relationship_samples = collect_samples(grouped, placement)
    write_schema(
        schema_path,
        placement,
        node_samples=node_samples,
        relationship_samples=relationship_samples,
        placement_statistics=placement_statistics,
    )
    placement_report = build_placement_report(
        placement_statistics,
        routing_layout=placement.routing_layout(),
        placement_strategy=placement.strategy_name,
    )
    placement_outputs = write_placement_outputs(
        placement_report_dir or graph_output.parent,
        placement_report,
        render_graph=render_placement_graph,
    )
    planning_db = graph_output.parent / "cache-plan.sqlite3"
    gpu_storage_result: dict[str, Any] | None = None
    gpu_distribution_raw: dict[str, Any] | None = None
    if "GPU" in placement.routing_layout().backends:
        gpu_distribution_raw = load_gpu_distribution_config(gpu_distribution_config)
        gpu_storage_root = (
            Path(gpu_distribution_output_dir).expanduser().resolve()
            if gpu_distribution_output_dir
            else graph_output.parent / "gpu-storage"
        )
        gpu_storage_result = plan_gpu_storage(
            graph_output,
            gpu_distribution_raw,
            output_dir=gpu_storage_root,
            routing_layout=placement.routing_layout(),
            planning_db_path=planning_db,
        )

    cache_result: dict[str, Any] | None = None
    cache_raw = load_cache_config(cache_config) if cache_config else None
    cache_root = Path(cache_output_dir).expanduser().resolve() if cache_output_dir else graph_output.parent / "cache"
    if cache_raw:
        cache_result = plan_graph_caches(
            graph_output,
            cache_raw,
            output_dir=cache_root,
            routing_layout=placement.routing_layout(),
            planning_db_path=planning_db,
        )

    if backend_config_output:
        write_backend_config_for_sources(
            placement.routing_layout().backends,
            backend_config_output,
            arango_database=arango_database,
            # Cache backends are selected by cache-plan.json at runtime. Keep
            # the domain backend configuration authoritative-data-only.
            cache_config=None,
            cache_root=None,
            gpu_distribution_config=gpu_distribution_raw,
            gpu_storage_plan=(gpu_storage_result or {}).get("plan"),
        )

    return {
        "input": str(source_dir),
        "output": str(graph_output),
        "schema": str(schema_path),
        "nodes": node_count,
        "relationships": relationship_count,
        "uuid_routing": list(placement.routing_layout().backends),
        "uuid_routing_bits": placement.routing_layout().bit_count,
        "id_payload_strategy": placement.payload_strategy_name,
        "placement": placement_outputs,
        "gpu_storage": gpu_storage_result,
        "cache": cache_result,
        "ignored_csv_groups": unknown,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert LDBC SNB Interactive v1 CsvBasic static+dynamic data to PolyGraph."
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
        choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"],
        default=list(DEFAULT_ROUTING_BACKENDS),
        help="Ordered backend list; B=3 automatically uses two ID MSB routing bits.",
    )
    parser.add_argument("--default-source", default="Neo4J", choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"])
    parser.add_argument("--arango-database", default="university")
    parser.add_argument("--cache-config", help="Optional GPU/Redis/mmap acceleration-cache configuration.")
    parser.add_argument("--cache-output-dir", help="Cache artifact directory; defaults to <graph-dir>/cache.")
    parser.add_argument(
        "--gpu-distribution-config",
        help="Physical distribution of authoritative GPU-owned vertices across GPU devices.",
    )
    parser.add_argument(
        "--gpu-distribution-output-dir",
        help="Authoritative GPU artifacts; defaults to <graph-dir>/gpu-storage.",
    )
    parser.add_argument(
        "--placement-report-dir",
        help=(
            "Directory for placement-report.json/.txt and placement-graph.dot/.svg/.pdf; "
            "defaults to the graph output directory."
        ),
    )
    parser.add_argument(
        "--no-placement-graph-render",
        action="store_true",
        help="Write the DOT source but skip Graphviz SVG/PDF rendering.",
    )
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
        placement_report_dir=args.placement_report_dir,
        render_placement_graph=not args.no_placement_graph_render,
        cache_config=args.cache_config,
        cache_output_dir=args.cache_output_dir,
        gpu_distribution_config=args.gpu_distribution_config,
        gpu_distribution_output_dir=args.gpu_distribution_output_dir,
    )
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
