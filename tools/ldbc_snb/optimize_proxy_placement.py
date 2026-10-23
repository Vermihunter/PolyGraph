#!/usr/bin/env python3
from __future__ import annotations

import argparse
import itertools
import json
import sqlite3
import sys
import tempfile
import time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.ldbc_snb.convert import discover_csv_files
from tools.ldbc_snb.fast_prepare import _fast_data_line_count
from tools.ldbc_snb.spec import EDGE_SPECS, NODE_SPECS

BACKENDS = ("Neo4J", "ArangoDB", "PostgreSQL")
NODE_TYPES = tuple(spec.type_name for spec in NODE_SPECS.values())
TYPE_INDEX = {name: index for index, name in enumerate(NODE_TYPES)}


def _first_two_fields(path: Path):
    with path.open("rb") as handle:
        handle.readline()
        for line in handle:
            if not line.strip():
                continue
            parts = line.split(b"|", 2)
            if len(parts) < 2:
                continue
            left = parts[0].strip()
            right = parts[1].strip()
            if left and right:
                yield left.decode("ascii"), right.decode("ascii")


def _node_counts(grouped: dict[str, list[Path]]) -> dict[str, int]:
    return {
        spec.type_name: sum(_fast_data_line_count(path) for path in grouped.get(prefix, []))
        for prefix, spec in NODE_SPECS.items()
    }


def _relationship_counts(grouped: dict[str, list[Path]]) -> dict[str, int]:
    return {
        spec.type_name: sum(_fast_data_line_count(path) for path in grouped.get(prefix, []))
        for prefix, spec in EDGE_SPECS.items()
    }


def _build_neighbor_mask_counts(grouped: dict[str, list[Path]], database: Path) -> dict[tuple[int, int], int]:
    db = sqlite3.connect(database)
    db.execute("PRAGMA journal_mode=OFF")
    db.execute("PRAGMA synchronous=OFF")
    db.execute("PRAGMA temp_store=FILE")
    db.execute("PRAGMA cache_size=-131072")
    db.execute(
        "CREATE TABLE node_masks(type_index INTEGER NOT NULL, node_id TEXT NOT NULL, mask INTEGER NOT NULL, "
        "PRIMARY KEY(type_index,node_id)) WITHOUT ROWID"
    )
    upsert = (
        "INSERT INTO node_masks(type_index,node_id,mask) VALUES(?,?,?) "
        "ON CONFLICT(type_index,node_id) DO UPDATE SET mask=(node_masks.mask | excluded.mask)"
    )

    batch: list[tuple[int, str, int]] = []
    edges = 0
    started = time.monotonic()
    last_report = started
    for prefix, spec in EDGE_SPECS.items():
        from_index = TYPE_INDEX[spec.from_type]
        to_index = TYPE_INDEX[spec.to_type]
        from_neighbor_bit = 1 << to_index
        to_neighbor_bit = 1 << from_index
        for path in grouped.get(prefix, []):
            for from_id, to_id in _first_two_fields(path):
                batch.append((from_index, from_id, from_neighbor_bit))
                batch.append((to_index, to_id, to_neighbor_bit))
                edges += 1
                if len(batch) >= 100_000:
                    db.executemany(upsert, batch)
                    db.commit()
                    batch.clear()
                now = time.monotonic()
                if now - last_report >= 5.0:
                    print(
                        f"[proxy-opt] scanned {edges:,} relationships ({now - started:,.1f}s)",
                        file=sys.stderr,
                        flush=True,
                    )
                    last_report = now
    if batch:
        db.executemany(upsert, batch)
        db.commit()

    counts = {
        (int(type_index), int(mask)): int(count)
        for type_index, mask, count in db.execute(
            "SELECT type_index, mask, count(*) FROM node_masks GROUP BY type_index, mask"
        )
    }
    db.close()
    return counts


def _proxy_copies(mask_counts: dict[tuple[int, int], int], assignment: tuple[int, ...]) -> int:
    total = 0
    for (type_index, mask), count in mask_counts.items():
        local_backend = assignment[type_index]
        remote_backends: set[int] = set()
        for neighbor_index in range(len(NODE_TYPES)):
            if mask & (1 << neighbor_index):
                backend = assignment[neighbor_index]
                if backend != local_backend:
                    remote_backends.add(backend)
        total += count * len(remote_backends)
    return total


def optimize(
    input_dir: Path,
    *,
    min_node_fraction: float,
    max_node_fraction: float,
    work_database: Path,
) -> tuple[dict, dict]:
    grouped = discover_csv_files(input_dir)
    node_counts = _node_counts(grouped)
    relationship_counts = _relationship_counts(grouped)
    mask_counts = _build_neighbor_mask_counts(grouped, work_database)

    total_nodes = sum(node_counts.values())
    target = total_nodes / len(BACKENDS)
    best_key = None
    best_assignment: tuple[int, ...] | None = None
    best_loads: tuple[int, ...] | None = None

    for assignment in itertools.product(range(len(BACKENDS)), repeat=len(NODE_TYPES)):
        if len(set(assignment)) != len(BACKENDS):
            continue
        loads = [0] * len(BACKENDS)
        for type_index, type_name in enumerate(NODE_TYPES):
            loads[assignment[type_index]] += node_counts.get(type_name, 0)
        fractions = [load / total_nodes if total_nodes else 0.0 for load in loads]
        if any(value < min_node_fraction or value > max_node_fraction for value in fractions):
            continue
        proxies = _proxy_copies(mask_counts, assignment)
        imbalance = int(sum(abs(load - target) for load in loads))
        spread = max(loads) - min(loads)
        key = (proxies, spread, imbalance, assignment)
        if best_key is None or key < best_key:
            best_key = key
            best_assignment = assignment
            best_loads = tuple(loads)

    if best_assignment is None or best_loads is None or best_key is None:
        raise RuntimeError(
            "No whole-type placement satisfies the requested node-balance constraints; "
            "relax --min-node-fraction/--max-node-fraction"
        )

    node_sources = {
        type_name: BACKENDS[best_assignment[type_index]]
        for type_index, type_name in enumerate(NODE_TYPES)
    }

    # The loader already writes an adjacency replica at each endpoint home. Put
    # the primary relationship on one endpoint home too, never on a third DB.
    # When endpoints differ, greedily choose the less-loaded primary-relation DB.
    primary_relation_load = defaultdict(int)
    relationship_sources: dict[str, str] = {}
    ordered_edges = sorted(
        EDGE_SPECS.values(),
        key=lambda spec: (-relationship_counts.get(spec.type_name, 0), spec.type_name),
    )
    for spec in ordered_edges:
        left = node_sources[spec.from_type]
        right = node_sources[spec.to_type]
        if left == right:
            chosen = left
        else:
            chosen = min((left, right), key=lambda backend: (primary_relation_load[backend], backend))
        relationship_sources[spec.type_name] = chosen
        primary_relation_load[chosen] += relationship_counts.get(spec.type_name, 0)

    config = {
        "storageBackends": list(BACKENDS),
        "strategy": "explicit",
        "nodeSources": node_sources,
        "relationshipSources": dict(sorted(relationship_sources.items())),
        "generatedBy": "tools/ldbc_snb/optimize_proxy_placement.py",
    }
    report = {
        "format": "polygraph.proxy-minimized-placement-analysis",
        "version": 1,
        "objective": "minimize exact proxy-node copies induced by cross-backend neighbor types",
        "constraints": {
            "allBackendsUsed": True,
            "minNodeFraction": min_node_fraction,
            "maxNodeFraction": max_node_fraction,
            "wholeNodeTypes": True,
            "relationshipPrimaryRestrictedToEndpointBackends": True,
        },
        "estimatedProxyCopies": best_key[0],
        "nodeCounts": node_counts,
        "relationshipCounts": relationship_counts,
        "backendNodeCounts": {BACKENDS[i]: best_loads[i] for i in range(len(BACKENDS))},
        "backendNodeFractions": {
            BACKENDS[i]: (best_loads[i] / total_nodes if total_nodes else 0.0)
            for i in range(len(BACKENDS))
        },
        "nodeSources": node_sources,
        "relationshipSources": dict(sorted(relationship_sources.items())),
        "primaryRelationshipCounts": dict(sorted(primary_relation_load.items())),
    }
    return config, report


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Find a balanced whole-schema SF placement that minimizes proxy-node copies."
    )
    parser.add_argument("--input", required=True, help="LDBC raw directory containing static/dynamic CSV shards")
    parser.add_argument("--output", required=True, help="Generated placement JSON")
    parser.add_argument("--report", help="Optional optimization report JSON")
    parser.add_argument("--min-node-fraction", type=float, default=0.03)
    parser.add_argument("--max-node-fraction", type=float, default=0.80)
    parser.add_argument("--work-db", help="Optional SQLite scratch DB path")
    args = parser.parse_args()

    if not 0 <= args.min_node_fraction <= args.max_node_fraction <= 1:
        parser.error("require 0 <= min-node-fraction <= max-node-fraction <= 1")

    source = Path(args.input).expanduser().resolve()
    output = Path(args.output).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    if args.work_db:
        work_db = Path(args.work_db).expanduser().resolve()
        work_db.parent.mkdir(parents=True, exist_ok=True)
        work_db.unlink(missing_ok=True)
        config, report = optimize(
            source,
            min_node_fraction=args.min_node_fraction,
            max_node_fraction=args.max_node_fraction,
            work_database=work_db,
        )
    else:
        with tempfile.TemporaryDirectory(prefix="polygraph-proxy-placement-") as tmp:
            config, report = optimize(
                source,
                min_node_fraction=args.min_node_fraction,
                max_node_fraction=args.max_node_fraction,
                work_database=Path(tmp) / "neighbor-masks.sqlite3",
            )

    output.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    report_path = Path(args.report).expanduser().resolve() if args.report else output.with_suffix(".report.json")
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"placement": str(output), "report": str(report_path), **report}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
