#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate a placement-comparison BFS request plan from one distribution's roots.")
    parser.add_argument("--roots", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--root-count", type=int, default=4)
    parser.add_argument("--max-depth", type=int, default=7)
    parser.add_argument("--direction", choices=["directed", "undirected", "both"], default="both")
    args = parser.parse_args()

    roots_path = Path(args.roots)
    with roots_path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))[: args.root_count]
    if len(rows) < args.root_count:
        raise ValueError(f"Need {args.root_count} roots in {roots_path}, found {len(rows)}")

    profile_name = f"d{args.max_depth}-{args.direction}"
    requests = []
    for root_index, row in enumerate(rows, start=1):
        requests.append({
            "id": f"root-{root_index:02d}-{profile_name}",
            "measure": True,
            "startUuid": str(row["uuid"]),
            "payload": {"maxDepth": args.max_depth, "direction": args.direction},
            "metadata": {"ldbcId": row.get("ldbc_id", ""), "profile": profile_name},
        })

    payload = {
        "mode": "serial",
        "defaults": {
            "endpoint": "/bfs",
            "method": "POST",
            "payload": {
                "returnPredecessors": False,
                "responseMode": "summary",
                "allowNativePushdown": False,
                # Reuse the no-cache Arango chunk sweep settings for every matrix case.
                "executionPolicy": "classic",
                "neighborBatchSize": 256,
                "neo4jMaxConcurrentRequests": 16,
                "arangoMaxConcurrentRequests": 16,
                "arangoRequestStrategy": "chunked",
                "arangoSingleRequestMaxFrontier": 50000,
                "streamingCandidateBatchSize": 16384,
                "redisPipelineBatchSize": 20000,
            },
        },
        "requests": requests,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(requests)} measured templates from {len(rows)} logical roots to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
