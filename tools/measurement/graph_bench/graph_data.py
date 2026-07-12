from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Optional


@dataclass(frozen=True)
class NodeRecord:
    uuid: str
    type: str
    source: str
    properties: dict[str, Any]


@dataclass(frozen=True)
class RelationshipRecord:
    uuid: str
    type: str
    source: str
    from_uuid: str
    to_uuid: str
    properties: dict[str, Any]


class GraphData:
    def __init__(self, raw: dict[str, Any]):
        self.raw = raw
        self.nodes = [
            NodeRecord(n["uuid"], n["type"], n.get("source", ""), n.get("properties", {}))
            for n in raw.get("nodes", [])
        ]
        self.relationships = [
            RelationshipRecord(r["uuid"], r["type"], r.get("source", ""), r["from"], r["to"], r.get("properties", {}))
            for r in raw.get("relationships", [])
        ]
        self.node_type_by_uuid = {n.uuid: n.type for n in self.nodes}

    @classmethod
    def load(cls, path: str | Path) -> "GraphData":
        with open(path, "r", encoding="utf-8") as f:
            return cls(json.load(f))

    def start_type(self, uuid: str) -> str:
        try:
            return self.node_type_by_uuid[uuid]
        except KeyError as exc:
            raise ValueError(f"Start UUID is not present in data file: {uuid}") from exc

    def uuids(self) -> list[str]:
        return [n.uuid for n in self.nodes]

    def write_data_with_source(self, output: str | Path, source: str) -> Path:
        clone = json.loads(json.dumps(self.raw))
        for node in clone.get("nodes", []):
            node["source"] = source
        for rel in clone.get("relationships", []):
            rel["source"] = source
        out = Path(output)
        out.parent.mkdir(parents=True, exist_ok=True)
        with open(out, "w", encoding="utf-8") as f:
            json.dump(clone, f)
        return out


def load_schema(path: str | Path) -> dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def write_schema_with_source(input_schema: str | Path, output: str | Path, source: str) -> Path:
    schema = load_schema(input_schema)
    for node_type in schema.get("nodeTypes", []):
        node_type["source"] = source
    for rel in schema.get("relations", []):
        rel["source"] = source
    out = Path(output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(schema, f)
    return out


def read_start_uuids(values: Iterable[str] | None, file: Optional[str | Path]) -> list[str]:
    result: list[str] = []
    if values:
        for value in values:
            result.extend([x.strip() for x in str(value).split(",") if x.strip()])
    if file:
        with open(file, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                result.extend([x.strip() for x in line.split(",") if x.strip()])
    # stable de-duplication
    seen = set()
    deduped = []
    for uuid in result:
        if uuid not in seen:
            seen.add(uuid)
            deduped.append(uuid)
    return deduped
