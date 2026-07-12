from __future__ import annotations

import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Optional


def repo_root() -> Path:
    """Return the repository root based on this package location."""
    # tools/measurement/graph_bench/common.py -> repo root is parents[3]
    return Path(__file__).resolve().parents[3]


def resolve_path(path: str | Path | None, *, base: Optional[Path] = None) -> Optional[Path]:
    if path is None or str(path) == "":
        return None
    p = Path(path).expanduser()
    if not p.is_absolute():
        p = (base or repo_root()) / p
    return p.resolve()


def ensure_dir(path: str | Path) -> Path:
    p = Path(path)
    p.mkdir(parents=True, exist_ok=True)
    return p


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def safe_file_part(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip())
    return value.strip("_") or "value"


def sanitize_db_identifier(name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not cleaned:
        cleaned = "X"
    if cleaned[0].isdigit():
        cleaned = f"X_{cleaned}"
    return cleaned


def parse_csv_list(values: str | Iterable[str] | None) -> list[str]:
    if values is None:
        return []
    if isinstance(values, str):
        raw = values.split(",")
    else:
        raw = []
        for item in values:
            raw.extend(str(item).split(","))
    return [x.strip() for x in raw if x.strip()]
