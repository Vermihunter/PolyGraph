#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shutil
import subprocess
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.measurement.graph_bench.metrics import MeasurementSink, RunRecord
from tools.measurement.graph_bench.plotting import write_plots

DERIVED_NAMES = {
    "measurements.csv",
    "measurements.jsonl",
    "summary.json",
    "plots",
    "log-reconstruction",
}


@dataclass(frozen=True)
class DatasetInput:
    target_name: str
    source_dir: Path


def _safe_name(value: str) -> str:
    rendered = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in value.strip())
    if not rendered or rendered in {".", ".."}:
        raise ValueError(f"Invalid dataset/run name: {value!r}")
    return rendered


def _parse_dataset_binding(value: str) -> DatasetInput:
    if "=" not in value:
        raise ValueError("--dataset must use TARGET_NAME=PATH")
    target, raw_path = value.split("=", 1)
    target = _safe_name(target)
    source = Path(raw_path).expanduser().resolve()
    if not source.is_dir():
        raise FileNotFoundError(source)
    if not (source / "measurements.jsonl").is_file() and not (source / "measurements.csv").is_file():
        raise ValueError(f"Dataset result directory has no measurements file: {source}")
    return DatasetInput(target_name=target, source_dir=source)


def _suite_inputs(path: str | Path) -> list[DatasetInput]:
    suite = Path(path).expanduser().resolve()
    datasets = suite / "datasets"
    if not datasets.is_dir():
        raise ValueError(f"Benchmark suite has no datasets/ directory: {suite}")
    result: list[DatasetInput] = []
    for child in sorted(datasets.iterdir()):
        if not child.is_dir():
            continue
        if (child / "measurements.jsonl").is_file() or (child / "measurements.csv").is_file():
            result.append(DatasetInput(target_name=_safe_name(child.name), source_dir=child))
    if not result:
        raise ValueError(f"Benchmark suite contains no mergeable dataset results: {suite}")
    return result


def _load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            stripped = line.strip()
            if not stripped:
                continue
            value = json.loads(stripped)
            if not isinstance(value, dict):
                raise ValueError(f"Expected JSON object in {path}:{line_number}")
            rows.append(value)
    return rows


def _optional_bool(value: Any) -> bool | None:
    if value is None or value == "":
        return None
    if isinstance(value, bool):
        return value
    rendered = str(value).strip().lower()
    if rendered in {"true", "1", "yes"}:
        return True
    if rendered in {"false", "0", "no"}:
        return False
    return None


def _load_csv(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            details_raw = row.pop("details_json", "") or "{}"
            rows.append(
                {
                    **row,
                    "repetition": int(row.get("repetition") or 0),
                    "seconds": float(row.get("seconds") or 0.0),
                    "same_as_reference": _optional_bool(row.get("same_as_reference")),
                    "vertex_count": int(row["vertex_count"]) if row.get("vertex_count") else None,
                    "details": json.loads(details_raw),
                }
            )
    return rows


def _load_records(source_dir: Path) -> list[dict[str, Any]]:
    jsonl = source_dir / "measurements.jsonl"
    if jsonl.is_file():
        return _load_jsonl(jsonl)
    return _load_csv(source_dir / "measurements.csv")


def _replace_path_prefix(value: Any, source_root: Path, target_root: Path) -> Any:
    if isinstance(value, dict):
        return {key: _replace_path_prefix(item, source_root, target_root) for key, item in value.items()}
    if isinstance(value, list):
        return [_replace_path_prefix(item, source_root, target_root) for item in value]
    if not isinstance(value, str) or not value:
        return value

    source_text = str(source_root)
    target_text = str(target_root)
    if value == source_text:
        return target_text
    if value.startswith(source_text + "/"):
        return target_text + value[len(source_text):]
    return value


def _normalize_record(record: dict[str, Any], source_root: Path, target_root: Path) -> RunRecord:
    rebased = _replace_path_prefix(record, source_root, target_root)
    details = rebased.get("details", {})
    if not isinstance(details, dict):
        details = {"value": details}
    return RunRecord(
        timestamp=str(rebased.get("timestamp", "")),
        scenario=str(rebased.get("scenario", "")),
        system=str(rebased.get("system", "")),
        phase=str(rebased.get("phase", "")),
        repetition=int(rebased.get("repetition") or 0),
        start_uuid=str(rebased.get("start_uuid", "")),
        seconds=float(rebased.get("seconds") or 0.0),
        status=str(rebased.get("status", "")),
        output_csv=str(rebased.get("output_csv", "")),
        expected_csv=str(rebased.get("expected_csv", "")),
        same_as_reference=_optional_bool(rebased.get("same_as_reference")),
        vertex_count=(
            int(rebased["vertex_count"])
            if rebased.get("vertex_count") not in {None, ""}
            else None
        ),
        details=details,
    )


def _record_key(record: RunRecord) -> tuple[str, str, str, int, str]:
    return (
        record.scenario,
        record.system,
        record.phase,
        record.repetition,
        record.start_uuid,
    )


def _select_record(current: RunRecord, candidate: RunRecord, policy: str) -> RunRecord:
    if policy == "keep-first":
        return current
    if policy == "keep-last":
        return candidate
    if policy == "error":
        raise ValueError(
            "Conflicting benchmark records for "
            f"{_record_key(candidate)}. Use --conflict-policy to resolve retries."
        )
    if policy != "prefer-ok":
        raise ValueError(f"Unknown conflict policy: {policy}")

    current_ok = current.status == "ok"
    candidate_ok = candidate.status == "ok"
    if current_ok and not candidate_ok:
        return current
    if candidate_ok and not current_ok:
        return candidate
    # For two successes (or two failures), later inputs represent the newer retry.
    return candidate


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _copy_file(source: Path, target: Path, policy: str) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        shutil.copy2(source, target)
        return
    if target.is_dir():
        raise ValueError(f"Cannot merge file over directory: {target}")
    if source.stat().st_size == target.stat().st_size and _sha256(source) == _sha256(target):
        return
    if policy == "error":
        raise ValueError(f"Artifact conflict while merging: {target}")
    if policy == "keep-first":
        return
    # keep-last and prefer-ok both favor the later retry's artifact.
    shutil.copy2(source, target)


def _copy_artifacts(source_dir: Path, target_dir: Path, policy: str) -> None:
    for source in sorted(source_dir.rglob("*")):
        relative = source.relative_to(source_dir)
        if relative.parts and relative.parts[0] in DERIVED_NAMES:
            continue
        target = target_dir / relative
        if source.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif source.is_file():
            _copy_file(source, target, policy)


def _write_aggregate_csv(output: Path, dataset_csvs: Iterable[tuple[str, Path]]) -> Path:
    output.parent.mkdir(parents=True, exist_ok=True)
    writer: csv.DictWriter[str] | None = None
    fieldnames: list[str] | None = None
    with output.open("w", encoding="utf-8", newline="") as handle:
        for dataset_name, source in dataset_csvs:
            with source.open("r", encoding="utf-8", newline="") as source_handle:
                reader = csv.DictReader(source_handle)
                if reader.fieldnames is None:
                    continue
                if writer is None:
                    fieldnames = ["dataset", *reader.fieldnames]
                    writer = csv.DictWriter(handle, fieldnames=fieldnames)
                    writer.writeheader()
                assert writer is not None and fieldnames is not None
                expected = fieldnames[1:]
                if reader.fieldnames != expected:
                    raise ValueError(
                        f"Measurement columns differ in {source}: {reader.fieldnames} != {expected}"
                    )
                for row in reader:
                    writer.writerow({"dataset": dataset_name, **row})
    if writer is None:
        output.write_text("dataset\n", encoding="utf-8")
    return output


def _event_logs_exist(dataset_dir: Path) -> bool:
    events = dataset_dir / "logs" / "events"
    return events.is_dir() and any(events.rglob("*.events.jsonl"))




def collect_merge_inputs(
    dataset_bindings: Sequence[str], suite_paths: Sequence[str | Path]
) -> list[DatasetInput]:
    inputs = [_parse_dataset_binding(value) for value in dataset_bindings]
    for suite in suite_paths:
        inputs.extend(_suite_inputs(suite))
    return inputs


def merge_benchmark_results(
    inputs: Sequence[DatasetInput],
    *,
    output_dir: str | Path,
    run_id: str,
    conflict_policy: str = "prefer-ok",
    reconstruct_logs: bool = False,
) -> Path:
    if not inputs:
        raise ValueError("At least one --dataset or --suite input is required")

    destination = Path(output_dir).expanduser().resolve()
    for item in inputs:
        source = item.source_dir.resolve()
        if destination == source or destination.is_relative_to(source):
            raise ValueError(
                f"Merge output must not be placed inside a source result directory: {source}"
            )
    if destination.exists() and any(destination.iterdir()):
        raise ValueError(f"Merge output directory is not empty: {destination}")
    destination.mkdir(parents=True, exist_ok=True)

    grouped: dict[str, list[Path]] = defaultdict(list)
    for item in inputs:
        grouped[_safe_name(item.target_name)].append(item.source_dir.resolve())

    dataset_csvs: list[tuple[str, Path]] = []
    dataset_summaries: list[dict[str, Any]] = []
    total_failed = 0
    total_mismatches = 0

    for dataset_name, source_dirs in sorted(grouped.items()):
        target_dir = destination / "datasets" / dataset_name
        target_dir.mkdir(parents=True, exist_ok=True)
        selected: dict[tuple[str, str, str, int, str], RunRecord] = {}
        source_record_counts: dict[str, int] = {}

        for source_dir in source_dirs:
            _copy_artifacts(source_dir, target_dir, conflict_policy)
            raw_records = _load_records(source_dir)
            source_record_counts[str(source_dir)] = len(raw_records)
            for raw in raw_records:
                candidate = _normalize_record(raw, source_dir, target_dir)
                key = _record_key(candidate)
                current = selected.get(key)
                selected[key] = candidate if current is None else _select_record(
                    current, candidate, conflict_policy
                )

        sink = MeasurementSink(target_dir)
        for key in sorted(selected):
            sink.add(selected[key])
        csv_path, jsonl_path, summary_path = sink.flush_all()
        for plot in write_plots(csv_path, target_dir / "plots"):
            print(f"Plot: {plot}")

        if reconstruct_logs and _event_logs_exist(target_dir):
            subprocess.run(
                [
                    sys.executable,
                    ROOT / "tools" / "measurement" / "log_reconstruction.py",
                    target_dir / "logs" / "events",
                    "--out-dir",
                    target_dir / "log-reconstruction",
                    "--inline-plotly",
                ],
                cwd=ROOT,
                check=True,
            )
        elif reconstruct_logs:
            print(f"Skipping log reconstruction for {dataset_name}: no PolyGraph event logs")

        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        failed = int(summary.get("failed_count", 0))
        mismatches = int(summary.get("comparison_failures", 0))
        total_failed += failed
        total_mismatches += mismatches
        start_uuids = sorted(
            {record.start_uuid for record in selected.values() if record.start_uuid}
        )
        dataset_summaries.append(
            {
                "name": dataset_name,
                "sources": [str(path) for path in source_dirs],
                "sourceRecordCounts": source_record_counts,
                "mergedRecordCount": len(selected),
                "startUuids": start_uuids,
                "measurements": str(csv_path),
                "events": str(jsonl_path),
                "summary": str(summary_path),
                "comparisonFailures": mismatches,
                "failedRecords": failed,
            }
        )
        dataset_csvs.append((dataset_name, csv_path))

    aggregate = _write_aggregate_csv(destination / "measurements.csv", dataset_csvs)
    status = "failed" if total_failed else "mismatch" if total_mismatches else "ok"
    summary = {
        "runId": _safe_name(run_id),
        "kind": "merged-benchmark-results",
        "status": status,
        "conflictPolicy": conflict_policy,
        "comparisonFailures": total_mismatches,
        "failedRecords": total_failed,
        "datasets": dataset_summaries,
        "aggregateMeasurements": str(aggregate),
    }
    summary_path = destination / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    manifest = {
        "runId": _safe_name(run_id),
        "output": str(destination),
        "conflictPolicy": conflict_policy,
        "reconstructLogs": reconstruct_logs,
        "inputs": [asdict(item) for item in inputs],
    }
    (destination / "merge-manifest.json").write_text(
        json.dumps(manifest, indent=2, default=str) + "\n", encoding="utf-8"
    )

    print(f"Merged benchmark output: {destination}")
    print(f"Summary:                 {summary_path}")
    print(f"Measurements:            {aggregate}")
    return destination


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Merge partial PolyGraph benchmark result directories without rerunning them."
    )
    parser.add_argument(
        "--dataset",
        action="append",
        default=[],
        metavar="TARGET=PATH",
        help=(
            "Merge one dataset result directory into TARGET. Repeat this option to combine "
            "resume runs into the same target dataset."
        ),
    )
    parser.add_argument(
        "--suite",
        action="append",
        default=[],
        help="Import every datasets/* result directory from a benchmark suite.",
    )
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument(
        "--conflict-policy",
        choices=["prefer-ok", "keep-first", "keep-last", "error"],
        default="prefer-ok",
        help="How to resolve duplicate scenario/system/phase/repetition/start-UUID records.",
    )
    parser.add_argument(
        "--reconstruct-logs",
        action="store_true",
        help="Regenerate log reconstruction only for datasets containing PolyGraph event logs.",
    )
    args = parser.parse_args(argv)

    try:
        inputs = collect_merge_inputs(args.dataset, args.suite)
        merge_benchmark_results(
            inputs,
            output_dir=args.output_dir,
            run_id=args.run_id,
            conflict_policy=args.conflict_policy,
            reconstruct_logs=args.reconstruct_logs,
        )
        return 0
    except (FileNotFoundError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
