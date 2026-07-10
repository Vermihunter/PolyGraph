from __future__ import annotations

import csv
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class CompareResult:
    same: bool
    expected_count: int
    actual_count: int
    missing_count: int
    extra_count: int
    wrong_distance_count: int
    diff_csv: str = ""


def read_bfs_csv(path: str | Path) -> dict[str, int]:
    p = Path(path)
    with open(p, "r", encoding="utf-8", newline="") as f:
        sample = f.read(4096)
        f.seek(0)
        delimiter = ";" if ";" in sample.splitlines()[0] else ","
        reader = csv.DictReader(f, delimiter=delimiter)
        if not reader.fieldnames:
            raise ValueError(f"CSV has no header: {path}")
        lower = {name.lower(): name for name in reader.fieldnames}
        vertex_col = lower.get("vertex_id") or lower.get("uuid") or reader.fieldnames[0]
        distance_col = lower.get("distance") or reader.fieldnames[1]
        rows: dict[str, int] = {}
        for row in reader:
            if not row.get(vertex_col):
                continue
            rows[row[vertex_col]] = int(row[distance_col])
        return rows


def sorted_bfs_items(rows: dict[str, int]) -> list[tuple[str, int]]:
    return sorted(rows.items(), key=lambda x: (x[1], x[0]))


def compare_bfs_csv(expected: str | Path, actual: str | Path, diff_output: str | Path | None = None) -> CompareResult:
    exp = read_bfs_csv(expected)
    act = read_bfs_csv(actual)
    missing = sorted(set(exp) - set(act))
    extra = sorted(set(act) - set(exp))
    wrong = sorted(k for k in set(exp) & set(act) if exp[k] != act[k])

    diff_path = ""
    if diff_output and (missing or extra or wrong):
        out = Path(diff_output)
        out.parent.mkdir(parents=True, exist_ok=True)
        diff_path = str(out)
        with open(out, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(["kind", "vertex_id", "expected_distance", "actual_distance"])
            for vertex_id in missing:
                writer.writerow(["missing", vertex_id, exp[vertex_id], ""])
            for vertex_id in extra:
                writer.writerow(["extra", vertex_id, "", act[vertex_id]])
            for vertex_id in wrong:
                writer.writerow(["wrong_distance", vertex_id, exp[vertex_id], act[vertex_id]])

    return CompareResult(
        same=not missing and not extra and not wrong,
        expected_count=len(exp),
        actual_count=len(act),
        missing_count=len(missing),
        extra_count=len(extra),
        wrong_distance_count=len(wrong),
        diff_csv=diff_path,
    )
