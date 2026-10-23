#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.workflows.common import canonical_sf, discover_cache_configs, domain_dir, domain_manifest, polygraph, remove_path


def default_config_dir(sf: str) -> Path:
    key = canonical_sf(sf)[2:]
    whole, fraction = key.split("_", 1)
    short = f"sf{whole}" if fraction == "0" else f"sf{whole}_{fraction}"
    return ROOT / "config" / "cache" / "benchmark" / short


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate all benchmark cache plans from config/cache/benchmark/<sf>/*.json."
    )
    parser.add_argument("--sf", default="1")
    parser.add_argument("--distribution", default="schema-based")
    parser.add_argument("--config-dir")
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()

    manifest = domain_manifest(args.sf, args.distribution)
    if not manifest.is_file():
        raise FileNotFoundError(f"Prepared domain is missing: {manifest}")
    config_dir = Path(args.config_dir).expanduser().resolve() if args.config_dir else default_config_dir(args.sf)
    configs = discover_cache_configs(config_dir)
    root = domain_dir(args.sf, args.distribution)
    cache_root = root / "caches"
    planning_db = root / "cache-plan.sqlite3"
    if args.clean:
        remove_path(cache_root)
        remove_path(planning_db)

    polygraph(
        "cache", "build", manifest,
        "--config", *configs,
        "--output-dir", cache_root,
        "--planning-db", planning_db,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
