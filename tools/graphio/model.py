from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .id_codec import parse_global_id


@dataclass(frozen=True)
class NodeRecord:
    uuid: int
    type: str
    source: str
    properties: dict[str, Any]

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "NodeRecord":
        return cls(
            uuid=parse_global_id(value["uuid"]),
            type=str(value["type"]),
            source=str(value.get("source", "")),
            properties=dict(value.get("properties", {})),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "uuid": self.uuid,
            "type": self.type,
            "source": self.source,
            "properties": self.properties,
        }


@dataclass(frozen=True)
class RelationshipRecord:
    uuid: int
    type: str
    source: str
    from_uuid: int
    to_uuid: int
    properties: dict[str, Any]

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "RelationshipRecord":
        return cls(
            uuid=parse_global_id(value["uuid"]),
            type=str(value["type"]),
            source=str(value.get("source", "")),
            from_uuid=parse_global_id(value["from"]),
            to_uuid=parse_global_id(value["to"]),
            properties=dict(value.get("properties", {})),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "uuid": self.uuid,
            "type": self.type,
            "source": self.source,
            "from": self.from_uuid,
            "to": self.to_uuid,
            "properties": self.properties,
        }


@dataclass(frozen=True)
class GraphRecords:
    nodes: list[NodeRecord]
    relationships: list[RelationshipRecord]

    @property
    def node_index(self) -> dict[int, NodeRecord]:
        return {node.uuid: node for node in self.nodes}
