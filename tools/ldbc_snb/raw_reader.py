from __future__ import annotations

import json
from pathlib import Path
from typing import Iterator

from tools.graphio.model import NodeRecord, RelationshipRecord
from tools.graphio.reader import GraphReader


FORMAT = "polygraph.ldbc-csv"
VERSION = 1


class LdbcCsvGraphReader(GraphReader):
    """Lazy PolyGraph reader backed directly by official LDBC CsvBasic shards.

    ``fast_prepare.py`` writes only a compact descriptor.  The expensive graph
    records are reconstructed lazily from the original CSV files when a
    consumer actually needs them (persistent storage loading or cache
    materialization), so preparation itself does not create graph.jsonl.
    """

    def __init__(self, descriptor_path: str | Path):
        self.path = Path(descriptor_path).expanduser().resolve()
        raw = json.loads(self.path.read_text(encoding="utf-8"))
        if raw.get("format") != FORMAT or int(raw.get("version", 0)) != VERSION:
            raise ValueError(f"Unsupported LDBC CSV graph descriptor: {self.path}")
        self.raw = raw
        self.input_dir = Path(str(raw["input"])).expanduser().resolve()
        if not self.input_dir.is_dir():
            raise FileNotFoundError(
                f"LDBC CSV input referenced by {self.path} does not exist: {self.input_dir}"
            )

    def metadata(self) -> dict:
        excluded = {
            "format",
            "version",
            "input",
            "placement",
            "nodePrefixTable",
            "nodes",
            "relationships",
        }
        return {
            **{key: value for key, value in self.raw.items() if key not in excluded},
            "format": FORMAT,
            "version": VERSION,
            "sourceData": str(self.input_dir),
            "nodeCount": int(self.raw.get("nodes", 0)),
            "relationshipCount": int(self.raw.get("relationships", 0)),
        }

    def _placement(self):
        # Lazy import avoids a graphio -> LDBC converter import cycle during
        # normal JSON/JSONL reader initialization.
        from tools.ldbc_snb.convert import Placement

        placement_raw = self.raw.get("placement", {})
        placement = Placement(
            storage_backends=tuple(
                str(value)
                for value in placement_raw.get(
                    "storageBackends", self.raw.get("idRouting", [])
                )
            ),
            default_node_source=placement_raw.get("defaultNodeSource"),
            default_relationship_source=placement_raw.get(
                "defaultRelationshipSource"
            ),
            node_sources={
                str(key): str(value)
                for key, value in placement_raw.get("nodeSources", {}).items()
            },
            relationship_sources={
                str(key): str(value)
                for key, value in placement_raw.get(
                    "relationshipSources", {}
                ).items()
            },
        )
        placement.install_node_prefix_table(self.raw.get("nodePrefixTable", []))
        return placement

    def iter_nodes(self) -> Iterator[NodeRecord]:
        from tools.ldbc_snb.convert import discover_csv_files, iter_nodes

        grouped = discover_csv_files(self.input_dir)
        yield from iter_nodes(grouped, self._placement())

    def iter_relationships(self) -> Iterator[RelationshipRecord]:
        from tools.ldbc_snb.convert import discover_csv_files, iter_relationships

        grouped = discover_csv_files(self.input_dir)
        yield from iter_relationships(grouped, self._placement())
