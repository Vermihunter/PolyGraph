#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.graphio.domain import load_domain_manifest
from tools.graphio.workspace import workspace_for, claim_domain_output, claim_domain_index
from tools.workflows.common import canonical_sf, discover_cache_configs, domain_manifest, polygraph, remove_path


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
    domain = load_domain_manifest(manifest)
    workspace = workspace_for(domain)
    cache_root = workspace.path("caches")
    planning_db = workspace.path("index") / "cache-plan.sqlite3"
    claim_domain_output(domain, cache_root, protected=tuple(Path(path) for path in configs))
    claim_domain_index(domain, planning_db)
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
