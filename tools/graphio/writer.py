from __future__ import annotations

import json
from pathlib import Path
from typing import Iterable

from .model import NodeRecord, RelationshipRecord


class GraphJsonLinesWriter:
    def __init__(self, path: str | Path, metadata: dict | None = None):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._handle = self.path.open("w", encoding="utf-8", buffering=4 * 1024 * 1024)
        self.write_raw({
            "kind": "metadata",
            "format": "polygraph.graph.jsonl",
            "version": 1,
            "recordOrder": "nodes-then-relationships",
            **(metadata or {}),
        })

    def __enter__(self) -> "GraphJsonLinesWriter":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def close(self) -> None:
        if not self._handle.closed:
            self._handle.close()

    def write_raw(self, value: dict) -> None:
        self._handle.write(json.dumps(value, ensure_ascii=False, separators=(",", ":")))
        self._handle.write("\n")

    def write_node(self, node: NodeRecord) -> None:
        self.write_raw({"kind": "node", **node.to_dict()})

    def write_relationship(self, relationship: RelationshipRecord) -> None:
        self.write_raw({"kind": "relationship", **relationship.to_dict()})


def write_graph_json(
    path: str | Path,
    nodes: Iterable[NodeRecord],
    relationships: Iterable[RelationshipRecord],
    *,
    indent: int | None = None,
    metadata: dict | None = None,
) -> Path:
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        **(metadata or {}),
        "nodes": [node.to_dict() for node in nodes],
        "relationships": [relationship.to_dict() for relationship in relationships],
    }
    with output.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=indent)
    return output
