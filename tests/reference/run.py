"""Explicit CLI for enrolling, verifying, running, and comparing raw-ID oracles."""

from __future__ import annotations

import argparse
from collections import Counter
import csv
from dataclasses import asdict
import json
from pathlib import Path
import sys

from .bfs import bfs, project
from .compare import compare_results, read_result, write_result
from .dataset import (create_lock, file_digest, generate_queries, lock_digest, read_json,
                      validate_queries, verify_lock, write_json)
from .model import BfsResult, Projection, Vertex


def _projection_key(query: dict) -> tuple:
    return query["direction"], tuple(sorted(query["edge_types"])), tuple(sorted(query["target_types"]))


def _project(graph, query: dict) -> Projection:
    return project(graph, direction=query["direction"], edge_types=query["edge_types"],
                   target_types=query["target_types"])


def _corruption_probes(expected: BfsResult, projection: Projection, source: Vertex,
                       query: dict, actual_path: Path, output: Path) -> dict[str, bool]:
    """Deliberately break actual results; never replace the expected reference."""
    checks = {}

    def rejected(name: str, actual: BfsResult) -> None:
        result = compare_results(expected, actual, projection, source, max_depth=query["max_depth"],
                                 require_predecessors=query["return_predecessors"])
        if result.same:
            raise RuntimeError(f"Comparator accepted deliberate corruption: {name}")
        checks[name] = True

    distances = dict(expected.distance)
    del distances[source]
    rejected("missing_vertex", BfsResult(distances, dict(expected.predecessor)))
    distances = dict(expected.distance)
    raw_id = (1 << 64) - 1
    while ("Person", raw_id) in projection.vertices:
        raw_id -= 1
    distances[("Person", raw_id)] = 1
    rejected("extra_vertex", BfsResult(distances, dict(expected.predecessor)))
    distances = dict(expected.distance)
    distances[source] = 1
    rejected("wrong_distance", BfsResult(distances, dict(expected.predecessor)))
    parents = dict(expected.predecessor)
    parents[source] = source
    rejected("invalid_predecessor", BfsResult(dict(expected.distance), parents))

    corruptions = output / "corruptions"
    corruptions.mkdir(exist_ok=True)
    duplicate = corruptions / "deliberate-duplicate.csv"
    with actual_path.open(encoding="utf-8", newline="") as stream:
        rows = csv.reader(stream)
        header, first = next(rows), next(rows)
    with duplicate.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerows((header, first, first))
    try:
        read_result(duplicate, require_predecessors=query["return_predecessors"])
    except ValueError:
        checks["duplicate_row"] = True
    else:
        raise RuntimeError("Comparator accepted deliberate duplicate rows")
    return checks


def run_queries(dataset: Path, lock_path: Path, queries_path: Path, output: Path) -> dict:
    lock = read_json(lock_path)
    graph = verify_lock(dataset, lock)
    queries = read_json(queries_path)
    cases = validate_queries(queries, lock, graph)
    output = Path(output).resolve()
    dataset = Path(dataset).resolve()
    if output.is_relative_to(dataset) or dataset.is_relative_to(output):
        raise ValueError("Oracle output must be separate from the raw dataset")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError("Oracle output directory must be new or empty (stale reports are not reused)")
    output.mkdir(parents=True, exist_ok=True)
    report = {"format": "polygraph.reference.run", "version": 1,
              "dataset_lock_sha256": lock_digest(lock), "query_manifest_sha256": file_digest(queries_path),
              "snapshot_sha256": lock["snapshot_sha256"], "counts": lock["counts"],
              "query_count": len(cases), "results": [], "corruption_probes": {}}
    projections = {}
    for index, (query, source) in enumerate(cases, 1):
        print(f"[{index}/{len(cases)}] {query['name']}", flush=True)
        key = _projection_key(query)
        if key not in projections:
            projections[key] = _project(graph, query)
        projection = projections[key]
        expected = bfs(projection, source, max_depth=query["max_depth"],
                       return_predecessors=query["return_predecessors"])
        filename = query["name"] + ".csv"
        result_path = output / filename
        write_result(expected, result_path, include_predecessors=query["return_predecessors"])
        actual = read_result(result_path, require_predecessors=query["return_predecessors"])
        comparison = compare_results(expected, actual, projection, source, max_depth=query["max_depth"],
                                     require_predecessors=query["return_predecessors"])
        if not comparison.same:
            raise RuntimeError(f"Oracle result failed round-trip/parent validation: {query['name']}")
        histogram = Counter(expected.distance.values())
        report["results"].append({"name": query["name"], "source": {"vertex_type": source[0],
                                   "vertex_id": str(source[1])}, "query": query, "file": filename,
                                   "sha256": file_digest(result_path),
                                   "reached_vertices": len(expected.distance),
                                   "layer_sizes": {str(d): count for d, count in sorted(histogram.items())},
                                   "validated": True})
        if not report["corruption_probes"]:
            report["corruption_probes"] = _corruption_probes(expected, projection, source, query, result_path, output)
    write_json(output / "report.json", report)
    return report


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("lock", "verify", "roots", "run", "compare"):
        command = commands.add_parser(name)
        command.add_argument("--dataset", type=Path, required=True, help="Original static/ and dynamic/ CSV root")
        if name != "lock":
            command.add_argument("--lock", type=Path, required=True)
        if name in ("lock", "roots"):
            command.add_argument("--output", type=Path, required=True, help="New manifest; existing files are never overwritten")
        if name in ("run", "compare"):
            command.add_argument("--queries", type=Path, required=True)
        if name == "run":
            command.add_argument("--output-dir", type=Path, required=True)
        if name == "compare":
            command.add_argument("--query", required=True, help="Exact query name from the fixed manifest")
            command.add_argument("--actual", type=Path, required=True, help="Canonical typed raw-ID result CSV")
            command.add_argument("--report", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "lock":
            if args.output.exists():
                raise ValueError("Lock output already exists; verification never overwrites the enrolled snapshot")
            lock = create_lock(args.dataset)
            write_json(args.output, lock)
            print(json.dumps({"snapshot_sha256": lock["snapshot_sha256"], "counts": lock["counts"]}, sort_keys=True))
        elif args.command == "run":
            report = run_queries(args.dataset, args.lock, args.queries, args.output_dir)
            print(json.dumps({"queries": report["query_count"], "corruption_probes": report["corruption_probes"]}, sort_keys=True))
        else:
            lock = read_json(args.lock)
            graph = verify_lock(args.dataset, lock)
            if args.command == "verify":
                print(json.dumps({"verified": True, "snapshot_sha256": lock["snapshot_sha256"],
                                  "vertices": len(graph.vertices), "relationships": len(graph.edges)}, sort_keys=True))
            elif args.command == "roots":
                if args.output.exists():
                    raise ValueError("Query output already exists; fixed queries are never overwritten")
                manifest = generate_queries(graph, lock)
                write_json(args.output, manifest)
                print(json.dumps({"roots": len(manifest["roots"]), "queries": len(manifest["queries"])}, sort_keys=True))
            else:
                cases = validate_queries(read_json(args.queries), lock, graph)
                case = next(((query, source) for query, source in cases if query["name"] == args.query), None)
                if case is None:
                    raise ValueError(f"Unknown fixed query: {args.query}")
                query, source = case
                projection = _project(graph, query)
                expected = bfs(projection, source, max_depth=query["max_depth"], return_predecessors=query["return_predecessors"])
                actual = read_result(args.actual, require_predecessors=query["return_predecessors"])
                result = compare_results(expected, actual, projection, source, max_depth=query["max_depth"],
                                         require_predecessors=query["return_predecessors"])
                report = {"same": result.same, **asdict(result)}
                if args.report:
                    write_json(args.report, report)
                print(json.dumps(report, sort_keys=True))
                return 0 if result.same else 1
        return 0
    except (ValueError, OSError, RuntimeError, TypeError) as error:
        print(f"oracle: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
