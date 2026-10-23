#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.workflows.common import destroy_storage_project, polygraph, storage_marker, storage_projects


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Delete PolyGraph storage containers and their database volumes."
    )
    parser.add_argument("--sf", default="1")
    parser.add_argument("--distribution", default="schema-based")
    parser.add_argument(
        "--all", action="store_true",
        help="Delete every discovered PolyGraph SF storage project, not only the selected SF.",
    )
    parser.add_argument("--yes", action="store_true", help="Required destructive confirmation.")
    args = parser.parse_args()
    if not args.yes:
        parser.error("destructive reset requires --yes")

    if args.all:
        projects = storage_projects()
        if not projects:
            print("[storage] no PolyGraph storage containers found")
            return 0
        for project in projects:
            destroy_storage_project(project)
        for marker in (ROOT / "data" / "ldbc-snb").glob("sf*/**/.storage-loaded.json"):
            marker.unlink(missing_ok=True)
        return 0

    polygraph("instance", args.sf, args.distribution, "destroy", "--yes")
    storage_marker(args.sf, args.distribution).unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
