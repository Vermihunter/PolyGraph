#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.workflows.common import polygraph


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Activate one PolyGraph storage instance, preserving other SF volumes."
    )
    parser.add_argument("--sf", default="1")
    parser.add_argument("--distribution", default="schema-based")
    args = parser.parse_args()
    polygraph("instance", args.sf, args.distribution, "activate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
