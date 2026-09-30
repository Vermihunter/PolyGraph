from __future__ import annotations

import json
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Iterator

from .model import GraphRecords, NodeRecord, RelationshipRecord


class GraphReader(ABC):
    def metadata(self) -> dict:
        return {}

    @abstractmethod
    def iter_nodes(self) -> Iterator[NodeRecord]:
        raise NotImplementedError

    @abstractmethod
    def iter_relationships(self) -> Iterator[RelationshipRecord]:
        raise NotImplementedError

    def load(self) -> GraphRecords:
        return GraphRecords(list(self.iter_nodes()), list(self.iter_relationships()))


class JsonGraphReader(GraphReader):
    def __init__(self, path: Path):
        self.path = path

    def _raw(self) -> dict:
        with self.path.open("r", encoding="utf-8") as handle:
            value = json.load(handle)
        if not isinstance(value, dict):
            raise ValueError(f"Expected a JSON object in {self.path}")
        return value

    def metadata(self) -> dict:
        raw = self._raw()
        return {key: value for key, value in raw.items() if key not in {"nodes", "relationships"}}

    def iter_nodes(self) -> Iterator[NodeRecord]:
        for value in self._raw().get("nodes", []):
            yield NodeRecord.from_dict(value)

    def iter_relationships(self) -> Iterator[RelationshipRecord]:
        for value in self._raw().get("relationships", []):
            yield RelationshipRecord.from_dict(value)


class JsonLinesGraphReader(GraphReader):
    def __init__(self, path: Path):
        self.path = path

    def _records(self) -> Iterator[dict]:
        with self.path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, start=1):
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                try:
                    value = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise ValueError(f"Invalid JSONL record at {self.path}:{line_number}: {exc}") from exc
                if not isinstance(value, dict):
                    raise ValueError(f"JSONL record must be an object at {self.path}:{line_number}")
                yield value

    @staticmethod
    def _kind(value: dict) -> str:
        explicit = value.get("kind")
        if explicit:
            return str(explicit).lower()
        if "from" in value and "to" in value:
            return "relationship"
        if "uuid" in value and "type" in value:
            return "node"
        return "metadata"

    def metadata(self) -> dict:
        result: dict = {}
        for value in self._records():
            if self._kind(value) != "metadata":
                # PolyGraph JSONL writers place metadata first.  Stopping here
                # avoids an accidental full-file scan just to read the routing
                # layout on multi-hundred-GB prepared graphs.
                break
            payload = {key: item for key, item in value.items() if key != "kind"}
            result.update(payload)
        return result

    def iter_nodes(self) -> Iterator[NodeRecord]:
        ordered = self.metadata().get("recordOrder") == "nodes-then-relationships"
        for value in self._records():
            kind = self._kind(value)
            if ordered and kind in {"relationship", "edge"}:
                break
            if kind == "node":
                payload = {k: v for k, v in value.items() if k != "kind"}
                yield NodeRecord.from_dict(payload)

    def iter_relationships(self) -> Iterator[RelationshipRecord]:
        for value in self._records():
            if self._kind(value) in {"relationship", "edge"}:
                payload = {k: v for k, v in value.items() if k != "kind"}
                yield RelationshipRecord.from_dict(payload)


def _special_json_reader(path: Path) -> GraphReader | None:
    # Keep special graph formats behind a tiny JSON descriptor so callers can
    # continue using the GraphReader abstraction without materializing a second
    # copy of a large dataset.  Import lazily to avoid a graphio/LDBC cycle.
    try:
        with path.open("r", encoding="utf-8") as handle:
            raw = json.load(handle)
    except (json.JSONDecodeError, OSError):
        return None
    if not isinstance(raw, dict):
        return None
    if raw.get("format") == "polygraph.ldbc-csv":
        from tools.ldbc_snb.raw_reader import LdbcCsvGraphReader

        return LdbcCsvGraphReader(path)
    return None


def open_graph_reader(path: str | Path) -> GraphReader:
    resolved = Path(path)
    suffix = resolved.suffix.lower()
    if suffix in {".jsonl", ".ndjson"}:
        return JsonLinesGraphReader(resolved)
    if suffix == ".json":
        special = _special_json_reader(resolved)
        return special if special is not None else JsonGraphReader(resolved)
    # Content sniffing keeps old/custom filenames working.
    with resolved.open("r", encoding="utf-8") as handle:
        first = handle.read(4096).lstrip()
    if first.startswith("{"):
        special = _special_json_reader(resolved)
        if special is not None:
            return special
    return JsonGraphReader(resolved) if first.startswith("{") and '"nodes"' in first else JsonLinesGraphReader(resolved)


def load_graph_records(path: str | Path) -> GraphRecords:
    return open_graph_reader(path).load()
