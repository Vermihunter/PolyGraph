from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any, Iterable, Optional

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.graphio import BackendLayout, NodeRecord, RelationshipRecord, load_graph_records
from tools.graphio.id_codec import parse_global_id
from tools.graphio.writer import GraphJsonLinesWriter, write_graph_json


class GraphData:
    def __init__(self, nodes: list[NodeRecord], relationships: list[RelationshipRecord], source_path: Path | None = None):
        self.nodes = nodes
        self.relationships = relationships
        self.source_path = source_path
        self.node_type_by_uuid = {node.uuid: node.type for node in nodes}
        self.node_by_uuid = {node.uuid: node for node in nodes}

    @classmethod
    def load(cls, path: str | Path) -> "GraphData":
        records = load_graph_records(path)
        return cls(records.nodes, records.relationships, Path(path))

    @property
    def raw(self) -> dict[str, Any]:
        return {
            "nodes": [node.to_dict() for node in self.nodes],
            "relationships": [relationship.to_dict() for relationship in self.relationships],
        }

    def start_type(self, global_id: int | str) -> str:
        normalized = parse_global_id(global_id)
        try:
            return self.node_type_by_uuid[normalized]
        except KeyError as exc:
            raise ValueError(f"Start ID is not present in data file: {global_id}") from exc

    def uuids(self) -> list[int]:
        return [node.uuid for node in self.nodes]

    def write_data_with_source(self, output: str | Path, source: str) -> Path:
        """Create a single-backend graph; zero routing bits keep logical IDs stable."""
        routing_layout = BackendLayout((source,))
        nodes = [NodeRecord(node.uuid, node.type, source, dict(node.properties)) for node in self.nodes]
        relationships = [
            RelationshipRecord(
                relationship.uuid, relationship.type, source, relationship.from_uuid, relationship.to_uuid,
                dict(relationship.properties),
            )
            for relationship in self.relationships
        ]
        out = Path(output)
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.suffix.lower() in {".jsonl", ".ndjson"}:
            with GraphJsonLinesWriter(
                out, {"generated": "full-backend", "source": source, **routing_layout.metadata()}
            ) as writer:
                for node in nodes:
                    writer.write_node(node)
                for relationship in relationships:
                    writer.write_relationship(relationship)
        else:
            write_graph_json(out, nodes, relationships, metadata=routing_layout.metadata())
        return out



def load_schema(path: str | Path) -> dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def write_schema_with_source(input_schema: str | Path, output: str | Path, source: str) -> Path:
    schema = load_schema(input_schema)
    for node_type in schema.get("nodeTypes", []):
        node_type["source"] = source
    for relationship in schema.get("relations", []):
        relationship["source"] = source
    # Full-backend benchmark variants use a one-entry ID layout. With one backend
    # the MSB routing width is zero, so the original logical IDs remain valid.
    schema["idRouting"] = [source]
    schema["storageBackends"] = [source]
    schema["idRoutingBits"] = 0
    out = Path(output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8") as handle:
        json.dump(schema, handle, indent=2)
    return out


def read_start_uuids(values: Iterable[str] | None, file: Optional[str | Path]) -> list[str]:
    result: list[str] = []
    if values:
        for value in values:
            result.extend(item.strip() for item in str(value).split(",") if item.strip())
    if file:
        with open(file, "r", encoding="utf-8") as handle:
            first = True
            for line in handle:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                # Accept the CSV produced by select_bfs_roots.py as well as plain ID files.
                if first and line.lower().startswith(("uuid,", "id,")):
                    first = False
                    continue
                first = False
                result.append(line.split(",", 1)[0].strip())
    seen: set[str] = set()
    return [value for value in result if not (value in seen or seen.add(value))]
