"""Raw LDBC CsvBasic reconstruction for the legacy independent reference BFS."""
from __future__ import annotations
import json
from pathlib import Path
from tools.graphio.id_codec import BackendLayout, read_native_node_id_map, resolve_native_node_id
from .convert import Placement, csv_rows, discover_csv_files
from .spec import EDGE_SPECS, NODE_SPECS

def placement_from_schema(schema_path: str | Path) -> Placement:
    """Reconstruct schema-based authoritative placement without reading graph.jsonl.

    The generated LDBC graph-shape schema contains the authoritative source for
    every node and relationship type.  That is sufficient for schema-based data
    because PolyGraph's owner bits are backend-name based.  A genuinely
    per-object distributed placement cannot be reconstructed from the schema
    alone, so fail loudly rather than generating incorrect reference IDs.
    """
    path = Path(schema_path).expanduser().resolve()
    raw = json.loads(path.read_text(encoding="utf-8"))

    node_sources: dict[str, str] = {}
    relationship_sources: dict[str, str] = {}
    used_sources: set[str] = set()
    distributed: list[str] = []

    def source_for(record: dict, kind: str) -> str:
        name = str(record.get("name", "<unknown>"))
        source = str(record.get("source", "")).strip()
        if not source or source == "Distributed":
            distributed.append(f"{kind}:{name}")
            return ""
        used_sources.add(source)
        return source

    for record in raw.get("nodeTypes", []):
        source = source_for(record, "node")
        if source:
            node_sources[str(record["name"])] = source

    for record in raw.get("relations", []):
        source = source_for(record, "relationship")
        if source:
            relationship_sources[str(record["name"])] = source

    if distributed:
        examples = ", ".join(distributed[:5])
        suffix = " ..." if len(distributed) > 5 else ""
        raise ValueError(
            "Raw-CSV reference BFS cannot reconstruct per-object distributed placement "
            f"from the graph schema alone ({examples}{suffix}). Use a schema-based domain "
            "with explicit type placement or extend the reference configuration with the "
            "original placement routing order."
        )
    if not used_sources:
        raise ValueError(f"Graph schema contains no authoritative placement sources: {path}")

    layout = BackendLayout.from_sources(used_sources)
    return Placement(
        storage_backends=layout.backends,
        node_sources=node_sources,
        relationship_sources=relationship_sources,
    )


def load_ldbc_snb_graph(
    input_dir: str | Path,
    *,
    schema_path: str | Path,
    native_id_map_path: str | Path | None = None,
    undirected: bool = True,
    graph_factory=None,
):
    """Load an LDBC SNB CsvBasic snapshot directly into reference adjacency.

    No PolyGraph JSON/JSONL data is read.  IDs are regenerated using the same
    bounded-memory node payload allocation as the LDBC converter.  Nodes must be
    visited before relationships because the converter assigns compact prefix
    codes in that deterministic order as well.
    """
    source = Path(input_dir).expanduser().resolve()
    if not source.is_dir():
        raise FileNotFoundError(f"LDBC CsvBasic input directory does not exist: {source}")

    grouped = discover_csv_files(source)
    placement = placement_from_schema(schema_path)
    native_ids = read_native_node_id_map(native_id_map_path)
    if graph_factory is None:
        from tools.algorithms.bfs import Graph
        graph_factory = Graph
    graph = graph_factory()

    # First pass: reproduce the converter's deterministic node-prefix allocation.
    for prefix, spec in NODE_SPECS.items():
        for path in grouped.get(prefix, []):
            for _header, row in csv_rows(path):
                if not row:
                    continue
                external_id = row[0].strip()
                if not external_id:
                    continue
                node_uuid, _ = placement.node_route(spec.type_name, external_id)
                graph.add_node_uuid(resolve_native_node_id(node_uuid, native_ids))

    # Second pass: construct adjacency directly from the original relationships.
    for prefix, spec in EDGE_SPECS.items():
        for path in grouped.get(prefix, []):
            for _header, row in csv_rows(path):
                if len(row) < 2:
                    continue
                from_id = row[0].strip()
                to_id = row[1].strip()
                if not from_id or not to_id:
                    continue
                from_uuid, _ = placement.node_route(spec.from_type, from_id)
                to_uuid, _ = placement.node_route(spec.to_type, to_id)
                graph.add_edge(
                    resolve_native_node_id(from_uuid, native_ids),
                    resolve_native_node_id(to_uuid, native_ids),
                    undirected=undirected,
                )

    return graph

