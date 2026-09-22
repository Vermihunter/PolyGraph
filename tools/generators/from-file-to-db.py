#!/usr/bin/env python3
"""Compatibility wrapper around the canonical integer-ID graph loader.

The old implementation duplicated the loader and stored a uuid property on every
Neo4j entity. Keeping one loader is important now that real Neo4j entities use explicit integer localId properties, proxies retain numeric uuid,
and ArangoDB uses uint64 IDs.
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.graphio.id_codec import routing_names
from tools.graphio.loaders import load_graph
from tools.graphio.reader import open_graph_reader


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Load PolyGraph JSON/JSONL data using uint64 MSB routing, Neo4j localId integers, "
            "numeric proxy uuid properties, indexed ArangoDB uuid properties, and PostgreSQL relational storage."
        )
    )
    parser.add_argument("input_graph", help="Path to graph .json, .jsonl, or .ndjson")
    parser.add_argument("--batch-size", type=int, default=5000)
    parser.add_argument("--reset", action="store_true")
    parser.add_argument(
        "--only-source", choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"], action="append", default=[]
    )
    parser.add_argument("--resolved-output")
    parser.add_argument("--id-map-output")
    parser.add_argument("--arango-url", default="http://localhost:8529")
    parser.add_argument("--arango-user", default="root")
    parser.add_argument("--arango-password", default="rootpassword")
    parser.add_argument("--arango-db", default="university")
    parser.add_argument("--neo4j-uri", default="bolt://localhost:7687", help="Kept for CLI compatibility.")
    parser.add_argument("--neo4j-http", default="http://localhost:7474/db/neo4j/tx/commit")
    parser.add_argument("--neo4j-user", default="neo4j")
    parser.add_argument("--neo4j-password", default="password")
    parser.add_argument("--postgres-endpoint", default="127.0.0.1:5432")
    parser.add_argument("--postgres-db", default="polygraph")
    parser.add_argument("--postgres-user", default="polygraph")
    parser.add_argument("--postgres-password", default="polygraph")
    args = parser.parse_args()

    if args.batch_size <= 0:
        parser.error("--batch-size must be positive")

    graph_metadata = open_graph_reader(args.input_graph).metadata()
    layout = routing_names(graph_metadata)
    if not layout:
        parser.error("input graph must declare idRouting metadata")

    selected = set(args.only_source or layout)
    backends = []
    if "Neo4J" in layout:
        backends.append({
            "id": "Neo4J",
            "enabled": "Neo4J" in selected,
            "connection": {
                "endpoint": args.neo4j_http,
                "database": "neo4j",
                "credentials": {"username": args.neo4j_user, "password": args.neo4j_password},
            },
        })
    if "ArangoDB" in layout:
        backends.append({
            "id": "ArangoDB",
            "enabled": "ArangoDB" in selected,
            "connection": {
                "endpoint": f"{args.arango_url.rstrip('/')}/_db/{args.arango_db}/_api/cursor",
                "database": args.arango_db,
                "credentials": {"username": args.arango_user, "password": args.arango_password},
            },
        })
    if "PostgreSQL" in layout:
        backends.append({
            "id": "PostgreSQL",
            "kind": "PostgreSQL",
            "enabled": "PostgreSQL" in selected,
            "connection": {
                "endpoint": args.postgres_endpoint,
                "database": args.postgres_db,
                "credentials": {
                    "username": args.postgres_user,
                    "password": args.postgres_password,
                },
            },
        })
    if "GPU" in layout:
        backends.append({"id": "GPU", "enabled": "GPU" in selected})

    # read_id_routing_layout intentionally requires every route to be enabled,
    # even when --only-source limits the actual load. Keep all routes enabled in
    # the temporary config and pass the selection separately.
    for backend in backends:
        backend["enabled"] = True

    with tempfile.TemporaryDirectory(prefix="polygraph-loader-") as directory:
        config = Path(directory) / "backends.json"
        config.write_text(
            json.dumps({"version": 1, "idRouting": list(layout), "backends": backends}, indent=2) + "\n",
            encoding="utf-8",
        )
        result = load_graph(
            args.input_graph,
            config,
            reset=args.reset,
            batch_size=args.batch_size,
            only_sources=selected,
            resolved_output=args.resolved_output,
            id_map_output=args.id_map_output,
        )

    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
