#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
from collections import Counter
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Aggregate PolyGraph algorithm-analyzer JSONL output.")
    parser.add_argument("input")
    parser.add_argument("--output-prefix")
    args = parser.parse_args()

    source = Path(args.input)
    prefix = Path(args.output_prefix) if args.output_prefix else source.with_suffix("")
    records = []
    with source.open(encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                records.append(json.loads(line))

    breakdown: Counter[tuple] = Counter()
    totals = Counter()
    for record in records:
        totals["queries"] += 1
        totals["sourceNodeUses"] += int(record.get("sourceNodeUses", 0))
        totals["destinationNodeUses"] += int(record.get("destinationNodeUses", 0))
        totals["proxyNodeUses"] += int(record.get("proxyNodeUses", 0))
        totals["sourceProxyUses"] += int(record.get("sourceProxyUses", 0))
        totals["destinationProxyUses"] += int(record.get("destinationProxyUses", 0))
        for row in record.get("nodeBreakdown", []):
            key = (
                row.get("role"), row.get("type"), row.get("ownerBackend"),
                row.get("executionBackend"), bool(row.get("proxy")),
            )
            breakdown[key] += int(row.get("count", 0))

    summary = {
        "format": "polygraph.algorithm-analysis-summary",
        "version": 1,
        "totals": dict(totals),
        "breakdown": [
            {
                "role": key[0], "type": key[1], "ownerBackend": key[2],
                "executionBackend": key[3], "proxy": key[4], "count": count,
            }
            for key, count in sorted(breakdown.items())
        ],
    }
    json_path = prefix.with_suffix(".summary.json")
    csv_path = prefix.with_suffix(".summary.csv")
    json_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["role", "type", "ownerBackend", "executionBackend", "proxy", "count"])
        writer.writeheader()
        writer.writerows(summary["breakdown"])
    print(json.dumps({"records": len(records), "summary": str(json_path), "csv": str(csv_path), **dict(totals)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
