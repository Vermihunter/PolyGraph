#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.polygraph import resolve_compose_instance
from tools.workflows.common import (
    domain_manifest,
    load_json,
    polygraph,
    project_has_volumes,
    resumable_storage_load,
    run,
    storage_marker,
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Switch to an SF. Prepare it from raw data and load its three storage backends "
            "automatically when this SF has not been initialized yet."
        )
    )
    parser.add_argument("--sf", default="1")
    parser.add_argument("--distribution", default="schema-based")
    parser.add_argument("--input", help="Raw LDBC input override used only when preparation is needed.")
    parser.add_argument("--reload", action="store_true", help="Reload the databases even if this SF is already initialized.")
    parser.add_argument("--postgres-indexes", action="store_true")
    parser.add_argument("--batch-size", type=int, default=20000)
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()

    manifest = domain_manifest(args.sf, args.distribution)
    prepared_now = False
    if not manifest.is_file():
        prepare = [sys.executable, ROOT / "tools/ldbc_snb/prepare_schema.py", "--sf", args.sf]
        if args.input:
            prepare += ["--input", args.input]
        run(prepare)
        prepared_now = True

    instance = resolve_compose_instance(args.sf, args.distribution)
    marker = storage_marker(args.sf, args.distribution)
    graph = manifest.parent / "graph.jsonl"
    stale_marker = marker.is_file() and graph.is_file() and marker.stat().st_mtime < graph.stat().st_mtime
    marker_config_mismatch = False
    if marker.is_file():
        marker_state = load_json(marker)
        marker_config_mismatch = (
            marker_state.get("project") != instance.project
            or bool(marker_state.get("postgresTraversalIndexes", False)) != args.postgres_indexes
        )
    has_volumes = project_has_volumes(instance.project)
    needs_load = (
        args.reload
        or not marker.is_file()
        or stale_marker
        or marker_config_mismatch
        or not has_volumes
    )
    restart_load = (
        prepared_now or args.reload or stale_marker or marker_config_mismatch or not has_volumes
    )

    polygraph("instance", args.sf, args.distribution, "activate")
    if needs_load:
        resumable_storage_load(
            args.sf,
            args.distribution,
            project=instance.project,
            manifest=manifest,
            postgres_indexes=args.postgres_indexes,
            batch_size=args.batch_size,
            workers=args.workers,
            restart=restart_load,
        )
        print(f"[use-sf] initialized {instance.project}")
    else:
        print(f"[use-sf] reused initialized {instance.project}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
