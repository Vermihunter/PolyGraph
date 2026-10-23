#!/usr/bin/env python3
from __future__ import annotations

import html
import json
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

from tools.graphio.id_codec import BackendLayout
from tools.graphio.model import NodeRecord, RelationshipRecord
from tools.ldbc_snb.spec import EDGE_SPECS, NODE_SPECS


@dataclass
class PlacementStatistics:
    """Small streaming accumulator for generated entity placement.

    Cardinality is bounded by the number of logical types and backends, not by
    the dataset size. It is therefore safe to update while producing SF-scale
    JSONL output.
    """

    node_counts: dict[str, Counter[str]] = field(
        default_factory=lambda: defaultdict(Counter)
    )
    relationship_counts: dict[str, Counter[str]] = field(
        default_factory=lambda: defaultdict(Counter)
    )
    # (relationship type, relationship backend, source-node backend,
    #  target-node backend) -> relationship instances
    connection_counts: Counter[tuple[str, str, str, str]] = field(
        default_factory=Counter
    )

    def record_node(self, node: NodeRecord) -> None:
        self.node_counts[node.source][node.type] += 1

    def record_relationship(
        self,
        relationship: RelationshipRecord,
        routing_layout: BackendLayout,
    ) -> None:
        relationship_backend = relationship.source
        from_backend = routing_layout.decode_source(relationship.from_uuid)
        to_backend = routing_layout.decode_source(relationship.to_uuid)
        self.relationship_counts[relationship_backend][relationship.type] += 1
        self.connection_counts[
            (
                relationship.type,
                relationship_backend,
                from_backend,
                to_backend,
            )
        ] += 1

    @property
    def node_count(self) -> int:
        return sum(sum(type_counts.values()) for type_counts in self.node_counts.values())

    @property
    def relationship_count(self) -> int:
        return sum(
            sum(type_counts.values())
            for type_counts in self.relationship_counts.values()
        )

    def node_type_counts(self, type_name: str) -> dict[str, int]:
        return {
            backend: counts[type_name]
            for backend, counts in self.node_counts.items()
            if counts[type_name]
        }

    def relationship_type_counts(self, type_name: str) -> dict[str, int]:
        return {
            backend: counts[type_name]
            for backend, counts in self.relationship_counts.items()
            if counts[type_name]
        }


def _ordered_backends(
    configured: Iterable[str], statistics: PlacementStatistics
) -> list[str]:
    result = list(dict.fromkeys(str(value) for value in configured))
    observed = (
        set(statistics.node_counts)
        | set(statistics.relationship_counts)
        | {
            backend
            for _, relationship_backend, from_backend, to_backend in statistics.connection_counts
            for backend in (relationship_backend, from_backend, to_backend)
        }
    )
    result.extend(sorted(observed - set(result)))
    return result


def build_placement_report(
    statistics: PlacementStatistics,
    *,
    routing_layout: BackendLayout,
    placement_strategy: str,
    dataset: str = "LDBC SNB Interactive v1",
) -> dict[str, Any]:
    backends = _ordered_backends(routing_layout.backends, statistics)

    remote_relationship_count = 0
    remote_endpoint_count = 0
    cross_backend_endpoint_count = 0
    connection_rows: list[dict[str, Any]] = []
    for (
        relationship_type,
        relationship_backend,
        from_backend,
        to_backend,
    ), count in sorted(statistics.connection_counts.items()):
        if relationship_backend != from_backend or relationship_backend != to_backend:
            remote_relationship_count += count
        if relationship_backend != from_backend:
            remote_endpoint_count += count
        if relationship_backend != to_backend:
            remote_endpoint_count += count
        if from_backend != to_backend:
            cross_backend_endpoint_count += count
        connection_rows.append(
            {
                "relationshipType": relationship_type,
                "relationshipBackend": relationship_backend,
                "fromBackend": from_backend,
                "toBackend": to_backend,
                "count": count,
            }
        )

    backend_rows: dict[str, Any] = {}
    for backend in backends:
        nodes = dict(sorted(statistics.node_counts.get(backend, {}).items()))
        relationships = dict(
            sorted(statistics.relationship_counts.get(backend, {}).items())
        )
        backend_rows[backend] = {
            "nodeCount": sum(nodes.values()),
            "relationshipCount": sum(relationships.values()),
            "nodeTypes": nodes,
            "relationshipTypes": relationships,
        }

    node_type_rows: dict[str, Any] = {}
    for spec in NODE_SPECS.values():
        by_backend = statistics.node_type_counts(spec.type_name)
        node_type_rows[spec.type_name] = {
            "count": sum(by_backend.values()),
            "byBackend": dict(sorted(by_backend.items())),
        }

    relationship_type_rows: dict[str, Any] = {}
    for spec in EDGE_SPECS.values():
        by_backend = statistics.relationship_type_counts(spec.type_name)
        relationship_type_rows[spec.type_name] = {
            "count": sum(by_backend.values()),
            "from": spec.from_type,
            "to": spec.to_type,
            "directed": spec.directed,
            "byBackend": dict(sorted(by_backend.items())),
        }

    return {
        "format": "polygraph.placement-report",
        "version": 1,
        "dataset": dataset,
        "placementStrategy": placement_strategy,
        "storageBackends": backends,
        "summary": {
            "nodeCount": statistics.node_count,
            "relationshipCount": statistics.relationship_count,
            "entityCount": statistics.node_count + statistics.relationship_count,
            "relationshipsWithRemoteEndpoint": remote_relationship_count,
            "remoteRelationshipEndpointCount": remote_endpoint_count,
            "relationshipsWhoseEndpointsCrossBackends": cross_backend_endpoint_count,
        },
        "backends": backend_rows,
        "nodeTypes": node_type_rows,
        "relationshipTypes": relationship_type_rows,
        "connections": connection_rows,
    }


def _format_count(value: int) -> str:
    return f"{value:,}"


def render_text_report(report: dict[str, Any]) -> str:
    summary = report["summary"]
    lines = [
        str(report["dataset"]),
        "Placement report",
        "=" * 72,
        f"Strategy: {report['placementStrategy']}",
        f"Backends: {', '.join(report['storageBackends'])}",
        f"Nodes: {_format_count(summary['nodeCount'])}",
        f"Relationships: {_format_count(summary['relationshipCount'])}",
        (
            "Relationships with at least one endpoint outside their storage "
            f"backend: {_format_count(summary['relationshipsWithRemoteEndpoint'])}"
        ),
        "",
    ]

    for backend, values in report["backends"].items():
        lines.extend(
            [
                backend,
                "-" * len(backend),
                f"Nodes: {_format_count(values['nodeCount'])}",
            ]
        )
        if values["nodeTypes"]:
            for type_name, count in values["nodeTypes"].items():
                lines.append(f"  {type_name:<42} {_format_count(count):>14}")
        else:
            lines.append("  (none)")

        lines.append(f"Relationships: {_format_count(values['relationshipCount'])}")
        if values["relationshipTypes"]:
            for type_name, count in values["relationshipTypes"].items():
                lines.append(f"  {type_name:<42} {_format_count(count):>14}")
        else:
            lines.append("  (none)")
        lines.append("")

    lines.extend(["Observed backend connections", "-" * 28])
    if not report["connections"]:
        lines.append("(none)")
    for connection in report["connections"]:
        relationship = report["relationshipTypes"][connection["relationshipType"]]
        lines.append(
            f"{relationship['from']} [{connection['fromBackend']}] "
            f"-- {connection['relationshipType']} "
            f"[{connection['relationshipBackend']}] --> "
            f"{relationship['to']} [{connection['toBackend']}] "
            f"x {_format_count(connection['count'])}"
        )
    return "\n".join(lines) + "\n"


_BACKEND_COLORS = {
    "Neo4J": ("#dbeafe", "#2563eb"),
    "ArangoDB": ("#dcfce7", "#16a34a"),
    "GPU": ("#ffedd5", "#ea580c"),
    "Memgraph": ("#f3e8ff", "#9333ea"),
}


def _dot_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def _html(value: Any) -> str:
    return html.escape(str(value), quote=True)


def build_dot(report: dict[str, Any]) -> str:
    lines = [
        "digraph PlacementGraph {",
        '  graph [rankdir="LR", splines="spline", overlap="false", bgcolor="#f8fafc", pad="0.35", nodesep="0.35", ranksep="0.85", fontname="Helvetica"];',
        '  node [fontname="Helvetica", fontsize="10"];',
        '  edge [fontname="Helvetica", fontsize="9", arrowsize="0.72"];',
    ]

    node_ids: dict[tuple[str, str], str] = {}
    relationship_ids: dict[tuple[str, str], str] = {}

    for backend_index, backend in enumerate(report["storageBackends"]):
        values = report["backends"][backend]
        fill, border = _BACKEND_COLORS.get(backend, ("#f1f5f9", "#64748b"))
        cluster_label = (
            f"{backend}\\nNodes: {_format_count(values['nodeCount'])} | "
            f"Relationships: {_format_count(values['relationshipCount'])}"
        )
        lines.extend(
            [
                f"  subgraph cluster_{backend_index} {{",
                f"    label={_dot_string(cluster_label)};",
                f'    color="{border}";',
                f'    bgcolor="{fill}";',
                '    style="rounded,filled";',
                '    penwidth="2";',
                '    margin="18";',
            ]
        )

        for type_index, (type_name, count) in enumerate(values["nodeTypes"].items()):
            node_id = f"node_{backend_index}_{type_index}"
            node_ids[(backend, type_name)] = node_id
            label = (
                "<<TABLE BORDER='0' CELLBORDER='0' CELLSPACING='0' "
                "CELLPADDING='5'><TR><TD><B>"
                f"{_html(type_name)}</B></TD></TR><TR><TD>"
                f"{_html(_format_count(count))} nodes"
                "</TD></TR></TABLE>>"
            )
            lines.append(
                f'    {node_id} [shape="box", style="rounded,filled", '
                f'fillcolor="#ffffff", color="{border}", penwidth="1.4", label={label}];'
            )

        for type_index, (type_name, count) in enumerate(
            values["relationshipTypes"].items()
        ):
            relationship_id = f"relationship_{backend_index}_{type_index}"
            relationship_ids[(backend, type_name)] = relationship_id
            label = (
                "<<TABLE BORDER='0' CELLBORDER='0' CELLSPACING='0' "
                "CELLPADDING='5'><TR><TD><B>"
                f"{_html(type_name)}</B></TD></TR><TR><TD>"
                f"{_html(_format_count(count))} relationships"
                "</TD></TR></TABLE>>"
            )
            lines.append(
                f'    {relationship_id} [shape="hexagon", style="filled", '
                f'fillcolor="#ffffff", color="{border}", penwidth="1.4", label={label}];'
            )

        if not values["nodeTypes"] and not values["relationshipTypes"]:
            lines.append(
                f'    empty_{backend_index} [shape="plaintext", label="No generated entities"];'
            )
        lines.append("  }")

    # Aggregate arrow labels separately: a relationship may be stored once but
    # connect endpoints living in more than one backend under hash placement.
    for connection in report["connections"]:
        relationship_type = connection["relationshipType"]
        spec = report["relationshipTypes"][relationship_type]
        from_key = (connection["fromBackend"], spec["from"])
        relationship_key = (
            connection["relationshipBackend"],
            relationship_type,
        )
        to_key = (connection["toBackend"], spec["to"])
        from_id = node_ids.get(from_key)
        relationship_id = relationship_ids.get(relationship_key)
        to_id = node_ids.get(to_key)
        if not from_id or not relationship_id or not to_id:
            continue

        count = _format_count(connection["count"])
        first_remote = connection["fromBackend"] != connection["relationshipBackend"]
        second_remote = connection["toBackend"] != connection["relationshipBackend"]
        first_attrs = (
            'color="#7c3aed", style="dashed", penwidth="1.5"'
            if first_remote
            else 'color="#64748b"'
        )
        second_attrs = (
            'color="#7c3aed", style="dashed", penwidth="1.5"'
            if second_remote
            else 'color="#64748b"'
        )
        lines.append(
            f"  {from_id} -> {relationship_id} [{first_attrs}, label={_dot_string(count)}];"
        )
        arrow_direction = "forward" if spec["directed"] else "both"
        lines.append(
            f"  {relationship_id} -> {to_id} [{second_attrs}, dir={_dot_string(arrow_direction)}, label={_dot_string(count)}];"
        )

    lines.extend(
        [
            '  legend [shape="plaintext", label=<<TABLE BORDER="0" CELLBORDER="0" CELLSPACING="4"><TR><TD ALIGN="LEFT"><B>Legend</B></TD></TR><TR><TD ALIGN="LEFT"><FONT COLOR="#64748b">solid = local endpoint access</FONT></TD></TR><TR><TD ALIGN="LEFT"><FONT COLOR="#7c3aed">dashed purple = cross-backend endpoint access</FONT></TD></TR></TABLE>>];',
            "}",
        ]
    )
    return "\n".join(lines) + "\n"


def render_dot(
    dot_path: Path,
    *,
    formats: Iterable[str] = ("svg", "pdf"),
) -> tuple[dict[str, str], list[str]]:
    executable = shutil.which("dot")
    if not executable:
        return {}, [
            "Graphviz 'dot' was not found; placement-graph.dot was generated "
            "but SVG/PDF rendering was skipped."
        ]

    rendered: dict[str, str] = {}
    warnings: list[str] = []
    for output_format in formats:
        output_path = dot_path.with_suffix(f".{output_format}")
        try:
            subprocess.run(
                [executable, f"-T{output_format}", str(dot_path), "-o", str(output_path)],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            rendered[output_format] = str(output_path)
        except subprocess.CalledProcessError as exc:
            detail = (exc.stderr or exc.stdout or str(exc)).strip()
            warnings.append(
                f"Could not render {output_format.upper()} placement graph: {detail}"
            )
    return rendered, warnings


def write_placement_outputs(
    output_dir: str | Path,
    report: dict[str, Any],
    *,
    render_graph: bool = True,
) -> dict[str, Any]:
    target = Path(output_dir).expanduser().resolve()
    target.mkdir(parents=True, exist_ok=True)

    json_path = target / "placement-report.json"
    text_path = target / "placement-report.txt"
    dot_path = target / "placement-graph.dot"
    json_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    text_path.write_text(render_text_report(report), encoding="utf-8")
    dot_path.write_text(build_dot(report), encoding="utf-8")

    outputs: dict[str, Any] = {
        "json": str(json_path),
        "text": str(text_path),
        "dot": str(dot_path),
    }
    warnings: list[str] = []
    if render_graph:
        rendered, warnings = render_dot(dot_path)
        outputs.update(rendered)
    if warnings:
        outputs["warnings"] = warnings
        for warning in warnings:
            print(f"warning: {warning}", file=sys.stderr)
    return outputs
