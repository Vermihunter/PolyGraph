"""Run the independent oracle in an isolated, stdlib-only interpreter.

Only tests.reference is bootstrapped from this checkout. The unchanged Phase 2
independence tests inspect imports and run a fresh interpreter that forbids
production imports and production-file reads. Zero tests or any skip is failure.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys
import unittest


def run_suite(suite: unittest.TestSuite) -> int:
    if suite.countTestCases() == 0:
        print("ERROR: required oracle test selection is empty", file=sys.stderr)
        return 1
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() and not result.skipped else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--isolation-only", action="store_true")
    args = parser.parse_args()
    if not sys.flags.isolated or not sys.flags.no_site:
        parser.error("invoke with python3 -I -S to disable user paths and site packages")
    root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(root))
    loader = unittest.TestLoader()
    isolation = loader.loadTestsFromName("tests.reference.test_independence")
    if loader.errors or isolation.countTestCases() == 0:
        print("ERROR: required isolation tests are missing or could not be imported", file=sys.stderr)
        for error in loader.errors:
            print(error, file=sys.stderr)
        return 1
    if args.isolation_only:
        return run_suite(isolation)
    return run_suite(loader.discover(str(root / "tests/reference"), pattern="test_*.py", top_level_dir=str(root)))


if __name__ == "__main__":
    raise SystemExit(main())
