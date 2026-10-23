#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import random
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path
from typing import Iterable, Iterator

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.graphio.domain import write_backend_config_for_sources, write_domain_manifest, write_roots_csv
from tools.graphio.model import NodeRecord
from tools.ldbc_snb.convert import (
    EDGE_SPECS,
    NODE_SPECS,
    Placement,
    discover_csv_files,
    load_placement,
    write_schema,
)
from tools.ldbc_snb.placement_report import PlacementStatistics, build_placement_report, write_placement_outputs

DESCRIPTOR_FORMAT = "polygraph.ldbc-csv"
DESCRIPTOR_VERSION = 1


def _first_fields(path: Path) -> Iterator[str]:
    """Yield only the first pipe-delimited field, without CSV/property parsing."""
    with path.open("rb") as handle:
        handle.readline()  # header
        for line in handle:
            if not line.strip():
                continue
            value = line.split(b"|", 1)[0].strip()
            if value:
                yield value.decode("ascii")


def _fast_data_line_count(path: Path, *, chunk_size: int = 16 * 1024 * 1024) -> int:
    """Count official CsvBasic data records using block newline counting."""
    size = path.stat().st_size
    if size == 0:
        return 0
    with path.open("rb") as handle:
        header = handle.readline()
        if not header:
            return 0
        count = 0
        saw_data = False
        last_byte = b""
        while True:
            chunk = handle.read(chunk_size)
            if not chunk:
                break
            saw_data = True
            count += chunk.count(b"\n")
            last_byte = chunk[-1:]
        if saw_data and last_byte != b"\n":
            count += 1
        return count


def _require_explicit_schema_placement(placement: Placement) -> None:
    missing_nodes = [
        spec.type_name
        for spec in NODE_SPECS.values()
        if placement.configured_node_source(spec.type_name) is None
    ]
    missing_relationships = [
        spec.type_name
        for spec in EDGE_SPECS.values()
        if placement.configured_relationship_source(spec.type_name) is None
    ]
    if missing_nodes or missing_relationships:
        details = []
        if missing_nodes:
            details.append("node types: " + ", ".join(missing_nodes[:8]))
        if missing_relationships:
            details.append("relationship types: " + ", ".join(missing_relationships[:8]))
        raise ValueError(
            "Fast LDBC preparation requires schema/type placement (no per-record hash placement); "
            + "; ".join(details)
        )


def _scan_node_ids_and_roots(
    grouped: dict[str, list[Path]],
    placement: Placement,
    *,
    root_type: str,
    root_count: int,
    seed: int,
) -> tuple[dict[str, int], list[NodeRecord]]:
    rng = random.Random(seed)
    selected: list[tuple[str, str]] = []
    candidates = 0
    counts: dict[str, int] = defaultdict(int)
    started = time.monotonic()
    last_report = started

    for prefix, spec in NODE_SPECS.items():
        for path in grouped.get(prefix, []):
            for external_id in _first_fields(path):
                placement.register_node_prefix(spec.type_name, external_id)
                counts[spec.type_name] += 1
                if spec.type_name == root_type:
                    candidates += 1
                    if len(selected) < root_count:
                        selected.append((spec.type_name, external_id))
                    else:
                        replacement = rng.randrange(candidates)
                        if replacement < root_count:
                            selected[replacement] = (spec.type_name, external_id)
                now = time.monotonic()
                if now - last_report >= 5.0:
                    total = sum(counts.values())
                    print(
                        f"[fast-prepare] scanned {total:,} node IDs; "
                        f"{placement.node_prefix_count:,} ID prefixes "
                        f"({now - started:,.1f}s)",
                        file=sys.stderr,
                        flush=True,
                    )
                    last_report = now

    if not selected:
        raise ValueError(f"No nodes of type '{root_type}' exist in the LDBC snapshot")
    rng.shuffle(selected)
    roots: list[NodeRecord] = []
    for type_name, external_id in selected:
        global_id, source = placement.node_route(type_name, external_id)
        roots.append(
            NodeRecord(
                uuid=global_id,
                type=type_name,
                source=source,
                properties={"ldbc_id": int(external_id)},
            )
        )
    return dict(counts), roots


def _relationship_counts(grouped: dict[str, list[Path]]) -> dict[str, int]:
    result: dict[str, int] = {}
    started = time.monotonic()
    for prefix, spec in EDGE_SPECS.items():
        count = sum(_fast_data_line_count(path) for path in grouped.get(prefix, []))
        result[spec.type_name] = count
    print(
        f"[fast-prepare] counted {sum(result.values()):,} relationships "
        f"without parsing relationship properties ({time.monotonic() - started:,.1f}s)",
        file=sys.stderr,
        flush=True,
    )
    return result


def _placement_statistics_from_counts(
    placement: Placement,
    node_counts: dict[str, int],
    relationship_counts: dict[str, int],
) -> PlacementStatistics:
    stats = PlacementStatistics()
    for spec in NODE_SPECS.values():
        count = int(node_counts.get(spec.type_name, 0))
        if count:
            source = placement.configured_node_source(spec.type_name)
            assert source is not None
            stats.node_counts[source][spec.type_name] += count

    for spec in EDGE_SPECS.values():
        count = int(relationship_counts.get(spec.type_name, 0))
        if not count:
            continue
        relationship_source = placement.configured_relationship_source(spec.type_name)
        from_source = placement.configured_node_source(spec.from_type)
        to_source = placement.configured_node_source(spec.to_type)
        assert relationship_source is not None and from_source is not None and to_source is not None
        stats.relationship_counts[relationship_source][spec.type_name] += count
        stats.connection_counts[
            (spec.type_name, relationship_source, from_source, to_source)
        ] += count
    return stats


def _placement_payload(placement: Placement) -> dict:
    return {
        "storageBackends": list(placement.routing_layout().backends),
        "defaultNodeSource": placement.default_node_source,
        "defaultRelationshipSource": placement.default_relationship_source,
        "nodeSources": dict(placement.node_sources or {}),
        "relationshipSources": dict(placement.relationship_sources or {}),
    }


def _write_no_cache_plan(
    *,
    graph: Path,
    output_dir: Path,
    node_count: int,
    relationship_count: int,
    placement: Placement,
) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    residency = output_dir / "residency.bin"
    residency.write_bytes(b"")
    plan = {
        "format": "polygraph.cache-plan",
        "version": 1,
        "graph": str(graph.resolve()),
        "nodes": node_count,
        "relationships": relationship_count,
        "graphRouting": {
            "idRouting": list(placement.routing_layout().backends),
            "idRoutingBits": placement.routing_layout().metadata()["idRoutingBits"],
        },
        "locationEncoding": "external-residency-v1",
        "regions": [],
        "artifacts": {
            "residency": {
                "path": "residency.bin",
                "entries": 0,
                "bytes": 0,
                "record": "<uint64 canonical_id,uint64 cache_location_mask>",
            }
        },
        "priorities": {},
        "gpus": [],
        "mmap": {"enabled": False},
        "redis": {"enabled": False},
        "runtimeBackends": [],
        "fastPrepared": True,
    }
    plan_path = output_dir / "cache-plan.json"
    plan_path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    return plan_path


def _write_fast_validation(
    path: Path,
    *,
    graph: Path,
    node_counts: dict[str, int],
    relationship_counts: dict[str, int],
    placement: Placement,
) -> None:
    payload = {
        "graph": str(graph.resolve()),
        "valid": True,
        "mode": "fast-structural",
        "id_routing": list(placement.routing_layout().backends),
        "id_routing_bits": placement.routing_layout().metadata()["idRoutingBits"],
        "node_count": sum(node_counts.values()),
        "relationship_count": sum(relationship_counts.values()),
        "node_types": dict(sorted(node_counts.items())),
        "relationship_types": dict(sorted(relationship_counts.items())),
        "checks": [
            "node IDs are numeric and fit the deterministic PolyGraph prefix encoding",
            "schema placement is explicit for every LDBC node and relationship type",
            "record counts were read from the source CsvBasic shards",
        ],
        "not_checked": [
            "every relationship endpoint exists (deferred to loading/correctness validation)",
            "full property parsing (deferred until a consumer reads the raw descriptor)",
        ],
        "errors": [],
        "error_count": 0,
    }
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def prepare(args: argparse.Namespace) -> dict:
    source = Path(args.input).expanduser().resolve()
    output = Path(args.output_dir).expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    if not source.is_dir():
        raise FileNotFoundError(source)

    grouped = discover_csv_files(source)
    missing = [prefix for prefix in NODE_SPECS if prefix not in grouped]
    if missing:
        print(
            "warning: missing LDBC node groups: " + ", ".join(missing),
            file=sys.stderr,
        )

    placement = load_placement(
        args.placement_config,
        args.default_source,
        tuple(args.storage_backends),
    )
    _require_explicit_schema_placement(placement)

    print(
        f"[fast-prepare] source={source}; shards={sum(len(v) for v in grouped.values()):,}; "
        "graph.jsonl=DISABLED",
        file=sys.stderr,
        flush=True,
    )
    node_counts, roots = _scan_node_ids_and_roots(
        grouped,
        placement,
        root_type=args.start_type,
        root_count=args.root_count,
        seed=args.seed,
    )
    relationship_counts = _relationship_counts(grouped)
    stats = _placement_statistics_from_counts(placement, node_counts, relationship_counts)

    descriptor = output / "graph.ldbc.json"
    descriptor_payload = {
        "format": DESCRIPTOR_FORMAT,
        "version": DESCRIPTOR_VERSION,
        "dataset": "ldbc-snb-interactive-v1",
        "input": str(source),
        "idRoutingStrategy": placement.strategy_name,
        "idPayloadStrategy": placement.payload_strategy_name,
        **placement.routing_layout().metadata(),
        "placement": _placement_payload(placement),
        "nodePrefixTable": placement.export_node_prefix_table(),
        "nodes": sum(node_counts.values()),
        "relationships": sum(relationship_counts.values()),
        "recordOrder": "source-csv",
    }
    descriptor.write_text(json.dumps(descriptor_payload, indent=2) + "\n", encoding="utf-8")

    schema = output / "ldbc-snb.graph-shape.json"
    write_schema(schema, placement, placement_statistics=stats)

    placement_report = build_placement_report(
        stats,
        routing_layout=placement.routing_layout(),
        placement_strategy=placement.strategy_name,
    )
    placement_outputs = write_placement_outputs(
        output,
        placement_report,
        render_graph=not args.no_render_placement_graph,
    )

    backends = output / "backends.json"
    arango_database = args.arango_database or args.domain_id.replace("-", "_").replace(".", "_")
    write_backend_config_for_sources(
        placement.routing_layout().backends,
        backends,
        arango_database=arango_database,
    )

    roots_csv = write_roots_csv(output / "bfs_roots.csv", roots)
    (output / "start_uuids.txt").write_text(
        "".join(f"{root.uuid}\n" for root in roots), encoding="utf-8"
    )

    no_cache_plan = _write_no_cache_plan(
        graph=descriptor,
        output_dir=output / "caches" / "08-no-cache",
        node_count=sum(node_counts.values()),
        relationship_count=sum(relationship_counts.values()),
        placement=placement,
    )

    manifest = write_domain_manifest(
        output / "domain.json",
        domain_id=args.domain_id,
        name=f"LDBC SNB SF{args.sf} schema-based",
        description=(
            "LDBC SNB prepared directly from CsvBasic shards. The canonical data path is a "
            "lazy CSV descriptor; graph.jsonl was intentionally not materialized."
        ),
        schema=schema,
        data=descriptor,
        backends=backends,
        start_uuid=roots[0].uuid,
        cache_plan=no_cache_plan,
        max_depth=args.max_depth,
        direction=args.direction,
    )
    _write_fast_validation(
        output / "validation.json",
        graph=descriptor,
        node_counts=node_counts,
        relationship_counts=relationship_counts,
        placement=placement,
    )

    result = {
        "format": "polygraph.fast-ldbc-prepare",
        "version": 1,
        "input": str(source),
        "output": str(output),
        "domain": str(manifest),
        "graphDescriptor": str(descriptor),
        "graphJsonl": None,
        "schema": str(schema),
        "backends": str(backends),
        "cachePlan": str(no_cache_plan),
        "nodes": sum(node_counts.values()),
        "relationships": sum(relationship_counts.values()),
        "roots": str(roots_csv),
        "nodePrefixCount": placement.node_prefix_count,
        "placement": placement_outputs,
    }
    (output / "fast-prepare.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Prepare an LDBC SNB schema-placed PolyGraph domain without materializing graph.jsonl. "
            "The output graph.ldbc.json descriptor streams the original CsvBasic shards on demand."
        )
    )
    parser.add_argument("--input", required=True, help="Extracted LDBC CsvBasic directory")
    parser.add_argument("--sf", required=True, help="Scale factor label, e.g. 10")
    parser.add_argument("--domain-id", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--placement-config", required=True)
    parser.add_argument(
        "--storage-backends",
        nargs="+",
        default=["Neo4J", "ArangoDB", "PostgreSQL"],
    )
    parser.add_argument("--default-source", default="Neo4J")
    parser.add_argument("--arango-database")
    parser.add_argument("--start-type", default="Person")
    parser.add_argument("--root-count", type=int, default=100)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--max-depth", type=int, default=7)
    parser.add_argument("--direction", choices=["directed", "undirected", "both"], default="both")
    parser.add_argument("--no-render-placement-graph", action="store_true")
    args = parser.parse_args()
    if args.root_count <= 0:
        parser.error("--root-count must be positive")

    started = time.monotonic()
    result = prepare(args)
    result["durationSeconds"] = time.monotonic() - started
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
