#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.graphio import open_graph_reader
from tools.graphio.id_codec import BackendLayout, backend_id


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate a converted CPGDB LDBC SNB graph.")
    parser.add_argument("--graph", required=True, help="Converted .json/.jsonl graph")
    parser.add_argument("--report", help="Optional JSON validation report")
    args = parser.parse_args()

    node_types: Counter[str] = Counter()
    relationship_types: Counter[str] = Counter()
    sources: Counter[str] = Counter()
    node_directory: dict[int, tuple[str, str]] = {}
    errors: list[str] = []

    reader = open_graph_reader(args.graph)
    metadata = reader.metadata()
    configured_layout = metadata.get("idRouting")
    if not configured_layout:
        errors.append("graph metadata is missing idRouting")
        routing_layout = None
    else:
        routing_layout = BackendLayout(tuple(str(value) for value in configured_layout))

    declared_bits = metadata.get("idRoutingBits")
    if routing_layout is not None and declared_bits is not None and int(declared_bits) != routing_layout.bit_count:
        errors.append(
            f"idRoutingBits={declared_bits}, expected {routing_layout.bit_count} for {routing_layout.backends}"
        )

    for node in reader.iter_nodes():
        if node.uuid in node_directory:
            errors.append(f"duplicate node ID: {node.uuid}")
            continue
        node_directory[node.uuid] = (node.type, node.source)
        node_types[node.type] += 1
        sources[f"node:{node.source}"] += 1
        if routing_layout is not None:
            try:
                expected_backend = backend_id(node.source)
                actual_backend = routing_layout.decode_backend_id(node.uuid)
                if expected_backend != actual_backend:
                    errors.append(
                        f"node {node.uuid} source={node.source} encodes backend {actual_backend}, expected {expected_backend}"
                    )
            except ValueError as exc:
                errors.append(str(exc))

    relationship_uuids: set[int] = set()
    for relationship in reader.iter_relationships():
        if relationship.uuid in relationship_uuids:
            errors.append(f"duplicate relationship ID: {relationship.uuid}")
        relationship_uuids.add(relationship.uuid)
        relationship_types[relationship.type] += 1
        sources[f"relationship:{relationship.source}"] += 1
        for role, endpoint_uuid in (("from", relationship.from_uuid), ("to", relationship.to_uuid)):
            if endpoint_uuid not in node_directory:
                errors.append(
                    f"relationship {relationship.uuid} has missing {role} endpoint {endpoint_uuid}"
                )
        if routing_layout is not None:
            try:
                expected_backend = backend_id(relationship.source)
                actual_backend = routing_layout.decode_backend_id(relationship.uuid)
                if expected_backend != actual_backend:
                    errors.append(
                        f"relationship {relationship.uuid} source={relationship.source} encodes backend "
                        f"{actual_backend}, expected {expected_backend}"
                    )
            except ValueError as exc:
                errors.append(str(exc))

    report = {
        "graph": str(Path(args.graph).resolve()),
        "valid": not errors,
        "id_routing": list(routing_layout.backends) if routing_layout is not None else [],
        "id_routing_bits": routing_layout.bit_count if routing_layout is not None else None,
        "node_count": len(node_directory),
        "relationship_count": len(relationship_uuids),
        "node_types": dict(sorted(node_types.items())),
        "relationship_types": dict(sorted(relationship_types.items())),
        "sources": dict(sorted(sources.items())),
        "errors": errors[:100],
        "error_count": len(errors),
    }

    rendered = json.dumps(report, indent=2)
    if args.report:
        output = Path(args.report)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
