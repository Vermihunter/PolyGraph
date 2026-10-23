#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.workflows.common import (
    SCHEMA_PLACEMENT,
    canonical_sf,
    domain_dir,
    polygraph,
    raw_dir,
    remove_path,
    sf_value,
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Prepare one schema-based LDBC domain over Neo4j, ArangoDB and PostgreSQL."
    )
    parser.add_argument("--sf", default="1")
    parser.add_argument("--input", help="Raw LDBC directory; defaults to data/ldbc-snb/<sf>/raw.")
    parser.add_argument("--placement", default=str(SCHEMA_PLACEMENT))
    parser.add_argument("--root-count", type=int, default=100)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--clean", action="store_true", help="Delete only the prepared schema-based distribution first.")
    args = parser.parse_args()

    sf = canonical_sf(args.sf)
    output = domain_dir(sf)
    source = Path(args.input).expanduser().resolve() if args.input else raw_dir(sf)
    if not source.exists():
        raise FileNotFoundError(
            f"Raw LDBC input does not exist: {source}\n"
            f"Generate/copy the dataset there or pass --input explicitly."
        )
    if args.clean:
        remove_path(output)

    polygraph(
        "prepare", "ldbc",
        "--input", source,
        "--sf", sf_value(sf),
        "--domain-id", f"ldbc-snb-{sf}-schema-based",
        "--output-dir", output,
        "--placement-config", Path(args.placement).expanduser().resolve(),
        "--storage-backends", "Neo4J", "ArangoDB", "PostgreSQL",
        "--root-count", str(args.root_count),
        "--seed", str(args.seed),
        "--validation-mode", "basic",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
