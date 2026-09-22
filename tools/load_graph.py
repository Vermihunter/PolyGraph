#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import tempfile
from pathlib import Path

from graphio.loaders import load_graph


def main() -> int:
    parser = argparse.ArgumentParser(description="Load a PolyGraph JSON/JSONL graph into configured persistent backends.")
    parser.add_argument("input_graph")
    parser.add_argument("--backend-config")
    parser.add_argument("--batch-size", type=int, default=20_000)
    parser.add_argument("--workers", type=int, default=4, help="Parallel ArangoDB collection imports per batch.")
    parser.add_argument("--reset", action="store_true")
    parser.add_argument(
        "--resolved-output",
        help="Write the graph with explicit Neo4j localId integers resolved into global uint64 IDs.",
    )
    parser.add_argument(
        "--id-map-output",
        help="Write an old-ID to resolved-localId JSONL sidecar (defaults next to --resolved-output).",
    )
    parser.add_argument("--only-source", choices=["Neo4J", "ArangoDB", "PostgreSQL"], action="append", default=[])
    parser.add_argument("--arango-url", default="http://localhost:8529")
    parser.add_argument("--arango-user", default="root")
    parser.add_argument("--arango-password", default="rootpassword")
    parser.add_argument("--arango-db", default="university")
    parser.add_argument("--neo4j-uri", default="bolt://localhost:7687", help="Neo4j Bolt endpoint used for high-throughput bulk loading.")
    parser.add_argument("--neo4j-http", default="http://localhost:7474/db/neo4j/tx/commit")
    parser.add_argument("--neo4j-user", default="neo4j")
    parser.add_argument("--neo4j-password", default="password")
    args = parser.parse_args()

    temporary: tempfile.TemporaryDirectory[str] | None = None
    if args.backend_config:
        config_path = Path(args.backend_config).expanduser().resolve()
    else:
        temporary = tempfile.TemporaryDirectory(prefix="polygraph-loader-")
        config_path = Path(temporary.name) / "backends.json"
        config_path.write_text(
            json.dumps(
                {
                    "version": 1,
                    "idRouting": ["Neo4J", "ArangoDB"],
                    "backends": [
                        {
                            "id": "Neo4J",
                            "enabled": True,
                            "bolt_endpoint": args.neo4j_uri,
                            "connection": {
                                "endpoint": args.neo4j_http,
                                "database": "neo4j",
                                "credentials": {
                                    "username": args.neo4j_user,
                                    "password": args.neo4j_password,
                                },
                            },
                        },
                        {
                            "id": "ArangoDB",
                            "enabled": True,
                            "connection": {
                                "endpoint": f"{args.arango_url.rstrip('/')}/_db/{args.arango_db}/_api/cursor",
                                "database": args.arango_db,
                                "credentials": {
                                    "username": args.arango_user,
                                    "password": args.arango_password,
                                },
                            },
                        },
                    ],
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

    try:
        result = load_graph(
            args.input_graph,
            config_path,
            reset=args.reset,
            batch_size=args.batch_size,
            load_workers=args.workers,
            only_sources=args.only_source or None,
            resolved_output=args.resolved_output,
            id_map_output=args.id_map_output,
        )
        print(json.dumps(result, indent=2))
    finally:
        if temporary is not None:
            temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
