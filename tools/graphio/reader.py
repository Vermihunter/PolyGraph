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
                continue
            payload = {key: item for key, item in value.items() if key != "kind"}
            result.update(payload)
        return result

    def iter_nodes(self) -> Iterator[NodeRecord]:
        for value in self._records():
            if self._kind(value) == "node":
                payload = {k: v for k, v in value.items() if k != "kind"}
                yield NodeRecord.from_dict(payload)

    def iter_relationships(self) -> Iterator[RelationshipRecord]:
        for value in self._records():
            if self._kind(value) in {"relationship", "edge"}:
                payload = {k: v for k, v in value.items() if k != "kind"}
                yield RelationshipRecord.from_dict(payload)


def open_graph_reader(path: str | Path) -> GraphReader:
    resolved = Path(path)
    suffix = resolved.suffix.lower()
    if suffix in {".jsonl", ".ndjson"}:
        return JsonLinesGraphReader(resolved)
    if suffix == ".json":
        return JsonGraphReader(resolved)
    # Content sniffing keeps old/custom filenames working.
    with resolved.open("r", encoding="utf-8") as handle:
        first = handle.read(4096).lstrip()
    return JsonGraphReader(resolved) if first.startswith("{") and '"nodes"' in first else JsonLinesGraphReader(resolved)


def load_graph_records(path: str | Path) -> GraphRecords:
    return open_graph_reader(path).load()
