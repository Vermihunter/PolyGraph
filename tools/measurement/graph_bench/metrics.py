from __future__ import annotations

import csv
import json
import time
from contextlib import contextmanager
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Iterator, Optional

from .common import ensure_dir, utc_now_iso


@dataclass
class RunRecord:
    timestamp: str
    scenario: str
    system: str
    phase: str
    repetition: int
    start_uuid: str
    seconds: float
    status: str
    output_csv: str = ""
    expected_csv: str = ""
    same_as_reference: Optional[bool] = None
    vertex_count: Optional[int] = None
    details: dict[str, Any] = field(default_factory=dict)


class MeasurementSink:
    """Checkpointed collector for benchmark measurements and validation metadata."""

    def __init__(self, output_dir: str | Path, *, resume: bool = False):
        self.output_dir = ensure_dir(output_dir)
        self.records: list[RunRecord] = []
        self._record_indexes: dict[tuple[str, str, str, int, str], int] = {}
        if resume:
            self._load_existing()

    @staticmethod
    def _key(record: RunRecord) -> tuple[str, str, str, int, str]:
        return (
            record.scenario,
            record.system,
            record.phase,
            int(record.repetition),
            str(record.start_uuid),
        )

    def _load_existing(self) -> None:
        path = self.output_dir / "measurements.jsonl"
        if not path.is_file():
            return
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            if not line.strip():
                continue
            try:
                payload = json.loads(line)
                record = RunRecord(**payload)
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise ValueError(
                    f"Cannot resume from malformed measurement at {path}:{line_number}: {exc}"
                ) from exc
            self.add(record)

    def find(
        self,
        *,
        scenario: str,
        system: str,
        phase: str,
        repetition: int = 0,
        start_uuid: str = "",
    ) -> Optional[RunRecord]:
        key = (scenario, system, phase, int(repetition), str(start_uuid))
        index = self._record_indexes.get(key)
        return None if index is None else self.records[index]

    def successful(self, **key: Any) -> bool:
        record = self.find(**key)
        return record is not None and record.status == "ok"

    def remove(self, **key: Any) -> None:
        record = self.find(**key)
        if record is None:
            return
        removed_key = self._key(record)
        index = self._record_indexes.pop(removed_key)
        self.records.pop(index)
        self._record_indexes = {
            self._key(existing): existing_index
            for existing_index, existing in enumerate(self.records)
        }

    @contextmanager
    def measure(
        self,
        *,
        scenario: str,
        system: str,
        phase: str,
        repetition: int = 0,
        start_uuid: str = "",
        output_csv: str | Path | None = None,
        expected_csv: str | Path | None = None,
        details: Optional[dict[str, Any]] = None,
    ) -> Iterator[RunRecord]:
        started = time.perf_counter()
        record = RunRecord(
            timestamp=utc_now_iso(),
            scenario=scenario,
            system=system,
            phase=phase,
            repetition=repetition,
            start_uuid=start_uuid,
            seconds=0.0,
            status="running",
            output_csv=str(output_csv or ""),
            expected_csv=str(expected_csv or ""),
            details=details or {},
        )
        try:
            yield record
        except Exception as exc:
            record.status = "failed"
            record.details = {**record.details, "error": repr(exc)}
            raise
        else:
            record.status = "ok"
        finally:
            record.seconds = time.perf_counter() - started
            self.add(record)

    def add(self, record: RunRecord) -> None:
        key = self._key(record)
        existing_index = self._record_indexes.get(key)
        if existing_index is None:
            self._record_indexes[key] = len(self.records)
            self.records.append(record)
        else:
            self.records[existing_index] = record

    def write_jsonl(self, path: str | Path | None = None) -> Path:
        out = Path(path) if path else self.output_dir / "measurements.jsonl"
        out.parent.mkdir(parents=True, exist_ok=True)
        with open(out, "w", encoding="utf-8") as f:
            for record in self.records:
                f.write(json.dumps(asdict(record), sort_keys=True) + "\n")
        return out

    def write_csv(self, path: str | Path | None = None) -> Path:
        out = Path(path) if path else self.output_dir / "measurements.csv"
        out.parent.mkdir(parents=True, exist_ok=True)
        fields = [
            "timestamp", "scenario", "system", "phase", "repetition", "start_uuid",
            "seconds", "status", "output_csv", "expected_csv", "same_as_reference",
            "vertex_count", "details_json",
        ]
        with open(out, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            for record in self.records:
                row = asdict(record)
                row["details_json"] = json.dumps(row.pop("details"), sort_keys=True)
                writer.writerow(row)
        return out

    def write_summary(self, path: str | Path | None = None) -> Path:
        out = Path(path) if path else self.output_dir / "summary.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        summary: dict[str, Any] = {
            "record_count": len(self.records),
            "failed_count": sum(1 for r in self.records if r.status != "ok"),
            "comparison_failures": sum(1 for r in self.records if r.same_as_reference is False),
            "systems": sorted({r.system for r in self.records}),
            "scenarios": sorted({r.scenario for r in self.records}),
        }
        by_key: dict[str, list[float]] = {}
        for record in self.records:
            key = f"{record.scenario}/{record.system}/{record.phase}"
            by_key.setdefault(key, []).append(record.seconds)
        summary["timings"] = {
            key: {
                "runs": len(values),
                "total_seconds": sum(values),
                "mean_seconds": sum(values) / len(values),
                "min_seconds": min(values),
                "max_seconds": max(values),
            }
            for key, values in sorted(by_key.items())
        }
        with open(out, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
        return out

    def flush_all(self) -> tuple[Path, Path, Path]:
        return self.write_csv(), self.write_jsonl(), self.write_summary()
