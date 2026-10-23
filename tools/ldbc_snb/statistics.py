#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import sys
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Hashable, Iterable

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.ldbc_snb.convert import (
    DEFAULT_ROUTING_BACKENDS,
    Placement,
    csv_rows,
    discover_csv_files,
    edge_identity,
    load_placement,
    row_properties,
)
from tools.ldbc_snb.placement_report import (
    PlacementStatistics,
    build_placement_report,
    write_placement_outputs,
)
from tools.ldbc_snb.spec import AUXILIARY_PREFIXES, EDGE_SPECS, NODE_SPECS, EdgeSpec, NodeSpec


@dataclass(frozen=True)
class DegreeStatistics:
    population: int
    active: int
    total: int
    mean: float
    minimum: int
    median: int
    p95: int
    maximum: int
    stddev: float


@dataclass(frozen=True)
class TableStatistics:
    name: str
    kind: str
    sections: tuple[str, ...]
    files: int
    rows: int
    from_type: str | None = None
    to_type: str | None = None
    malformed_rows: int = 0


@dataclass(frozen=True)
class RelationshipStatistics:
    name: str
    from_type: str
    to_type: str
    directed: bool
    rows: int
    malformed_rows: int
    outgoing: DegreeStatistics
    incoming: DegreeStatistics
    combined: DegreeStatistics


def resolve_dataset_dir(input_path: str | Path) -> Path:
    candidate = Path(input_path).expanduser().resolve()
    if not candidate.is_dir():
        raise FileNotFoundError(f"LDBC dataset directory does not exist: {candidate}")
    if (candidate / "static").is_dir() and (candidate / "dynamic").is_dir():
        return candidate

    nested = candidate / "social_network"
    if (nested / "static").is_dir() and (nested / "dynamic").is_dir():
        return nested

    matches = sorted(
        parent
        for dynamic in candidate.glob("**/dynamic")
        if dynamic.is_dir() and (parent := dynamic.parent).joinpath("static").is_dir()
    )
    unique = list(dict.fromkeys(matches))
    if len(unique) == 1:
        return unique[0]
    if not unique:
        raise FileNotFoundError(
            f"Could not find sibling static/ and dynamic/ directories below {candidate}"
        )
    rendered = "\n".join(f"  {path}" for path in unique)
    raise ValueError(f"More than one LDBC dataset directory was found:\n{rendered}")


def _sections(dataset_dir: Path, paths: Iterable[Path]) -> tuple[str, ...]:
    values: set[str] = set()
    for path in paths:
        try:
            relative = path.relative_to(dataset_dir)
            values.add(relative.parts[0] if relative.parts else ".")
        except ValueError:
            values.add(path.parent.name)
    return tuple(sorted(values))


def _count_rows(paths: Iterable[Path]) -> int:
    return sum(1 for path in paths for _header, _row in csv_rows(path))


def _analyze_node_rows(
    paths: Iterable[Path],
    spec: NodeSpec,
    *,
    placement: Placement | None = None,
    placement_statistics: PlacementStatistics | None = None,
) -> int:
    """Count a node table and optionally collect exact backend placement.

    Placement collection follows the same routing function as conversion, but
    retains no node objects and writes no graph data.
    """

    rows = 0
    for path in paths:
        for _header, row in csv_rows(path):
            rows += 1
            if placement is None or placement_statistics is None:
                continue
            if not row:
                continue
            external_id = row[0].strip()
            if not external_id:
                continue
            _node_uuid, backend = placement.node_route(spec.type_name, external_id)
            placement_statistics.node_counts[backend][spec.type_name] += 1
    return rows


def _nearest_rank(
    degree_histogram: Counter[int], *, population: int, zero_count: int, quantile: float
) -> int:
    if population <= 0:
        return 0
    rank = max(1, math.ceil(quantile * population))
    if rank <= zero_count:
        return 0
    positive_rank = rank - zero_count
    cumulative = 0
    for degree in sorted(degree_histogram):
        cumulative += degree_histogram[degree]
        if cumulative >= positive_rank:
            return degree
    return max(degree_histogram, default=0)


def summarize_degrees(degrees: Counter[Hashable], population_hint: int) -> DegreeStatistics:
    active = len(degrees)
    population = max(population_hint, active)
    total = sum(degrees.values())
    if population == 0:
        return DegreeStatistics(0, 0, 0, 0.0, 0, 0, 0, 0, 0.0)

    zero_count = population - active
    histogram = Counter(degrees.values())
    mean = total / population
    sum_squares = sum((degree * degree) * frequency for degree, frequency in histogram.items())
    variance = max(0.0, (sum_squares / population) - (mean * mean))
    minimum = 0 if zero_count else min(histogram, default=0)

    return DegreeStatistics(
        population=population,
        active=active,
        total=total,
        mean=mean,
        minimum=minimum,
        median=_nearest_rank(histogram, population=population, zero_count=zero_count, quantile=0.50),
        p95=_nearest_rank(histogram, population=population, zero_count=zero_count, quantile=0.95),
        maximum=max(histogram, default=0),
        stddev=math.sqrt(variance),
    )


def _analyze_relationship(
    paths: list[Path],
    spec: EdgeSpec,
    *,
    source_population: int,
    target_population: int,
    placement: Placement | None = None,
    placement_statistics: PlacementStatistics | None = None,
) -> tuple[int, int, DegreeStatistics, DegreeStatistics, DegreeStatistics]:
    outgoing: Counter[str] = Counter()
    incoming: Counter[str] = Counter()
    combined: Counter[tuple[str, str]] = Counter()
    rows = 0
    malformed_rows = 0

    for path in paths:
        for header, row in csv_rows(path):
            rows += 1
            if len(row) < 2 or not row[0].strip() or not row[1].strip():
                malformed_rows += 1
                continue
            source_id = row[0].strip()
            target_id = row[1].strip()
            outgoing[source_id] += 1
            incoming[target_id] += 1
            combined[(spec.from_type, source_id)] += 1
            combined[(spec.to_type, target_id)] += 1

            if placement is not None and placement_statistics is not None:
                configured_source = placement.configured_relationship_source(spec.type_name)
                identity = None if configured_source is not None else edge_identity(
                    source_id, target_id, row[2:]
                )
                relationship_backend = placement.relationship_source(spec.type_name, identity)
                _source_uuid, source_backend = placement.node_route(
                    spec.from_type, source_id
                )
                _target_uuid, target_backend = placement.node_route(
                    spec.to_type, target_id
                )
                placement_statistics.relationship_counts[relationship_backend][
                    spec.type_name
                ] += 1
                placement_statistics.connection_counts[
                    (
                        spec.type_name,
                        relationship_backend,
                        source_backend,
                        target_backend,
                    )
                ] += 1

    combined_population = (
        source_population
        if spec.from_type == spec.to_type
        else source_population + target_population
    )

    return (
        rows,
        malformed_rows,
        summarize_degrees(outgoing, source_population),
        summarize_degrees(incoming, target_population),
        summarize_degrees(combined, combined_population),
    )


def analyze_dataset(
    input_dir: str | Path,
    *,
    placement_config: str | Path | None = None,
    default_source: str = "Neo4J",
    storage_backends: Iterable[str] = DEFAULT_ROUTING_BACKENDS,
) -> dict[str, Any]:
    dataset_dir = resolve_dataset_dir(input_dir)
    grouped = discover_csv_files(dataset_dir)
    if not grouped:
        raise ValueError(f"No CSV files were found below {dataset_dir}")

    table_results: dict[str, TableStatistics] = {}
    node_rows_by_type: dict[str, int] = {}
    warnings: list[str] = []
    placement = (
        load_placement(
            str(Path(placement_config).expanduser().resolve()),
            default_source,
            tuple(storage_backends),
        )
        if placement_config is not None
        else None
    )
    placement_statistics = PlacementStatistics() if placement is not None else None

    # Count nodes first so relationship degree means/minima include zero-degree nodes.
    for prefix, spec in NODE_SPECS.items():
        paths = grouped.get(prefix, [])
        if not paths:
            continue
        rows = _analyze_node_rows(
            paths,
            spec,
            placement=placement,
            placement_statistics=placement_statistics,
        )
        node_rows_by_type[spec.type_name] = rows
        table_results[prefix] = TableStatistics(
            name=prefix,
            kind="node",
            sections=_sections(dataset_dir, paths),
            files=len(paths),
            rows=rows,
        )

    relationship_results: list[RelationshipStatistics] = []
    for prefix, spec in EDGE_SPECS.items():
        paths = grouped.get(prefix, [])
        if not paths:
            continue
        source_population = node_rows_by_type.get(spec.from_type, 0)
        target_population = node_rows_by_type.get(spec.to_type, 0)
        rows, malformed_rows, outgoing, incoming, combined = _analyze_relationship(
            paths,
            spec,
            source_population=source_population,
            target_population=target_population,
            placement=placement,
            placement_statistics=placement_statistics,
        )
        if source_population == 0:
            warnings.append(
                f"{prefix}: missing {spec.from_type} node table; outgoing population uses active IDs"
            )
        if target_population == 0:
            warnings.append(
                f"{prefix}: missing {spec.to_type} node table; incoming population uses active IDs"
            )
        if outgoing.active > source_population > 0:
            warnings.append(
                f"{prefix}: {outgoing.active} source IDs exceed {source_population} {spec.from_type} rows"
            )
        if incoming.active > target_population > 0:
            warnings.append(
                f"{prefix}: {incoming.active} target IDs exceed {target_population} {spec.to_type} rows"
            )
        if malformed_rows:
            warnings.append(f"{prefix}: ignored {malformed_rows} malformed relationship rows")

        table_results[prefix] = TableStatistics(
            name=prefix,
            kind="relationship",
            sections=_sections(dataset_dir, paths),
            files=len(paths),
            rows=rows,
            from_type=spec.from_type,
            to_type=spec.to_type,
            malformed_rows=malformed_rows,
        )
        relationship_results.append(
            RelationshipStatistics(
                name=prefix,
                from_type=spec.from_type,
                to_type=spec.to_type,
                directed=spec.directed,
                rows=rows,
                malformed_rows=malformed_rows,
                outgoing=outgoing,
                incoming=incoming,
                combined=combined,
            )
        )

    for prefix, paths in grouped.items():
        if prefix in table_results:
            continue
        kind = "auxiliary" if prefix in AUXILIARY_PREFIXES else "unknown"
        table_results[prefix] = TableStatistics(
            name=prefix,
            kind=kind,
            sections=_sections(dataset_dir, paths),
            files=len(paths),
            rows=_count_rows(paths),
        )
        if kind == "unknown":
            warnings.append(f"unrecognized CSV table: {prefix}")

    tables = [table_results[name] for name in sorted(table_results)]
    relationships = sorted(relationship_results, key=lambda item: item.name)
    summary = {
        "csv_files": sum(table.files for table in tables),
        "logical_tables": len(tables),
        "rows": sum(table.rows for table in tables),
        "node_rows": sum(table.rows for table in tables if table.kind == "node"),
        "relationship_rows": sum(
            table.rows - table.malformed_rows for table in tables if table.kind == "relationship"
        ),
        "auxiliary_rows": sum(table.rows for table in tables if table.kind == "auxiliary"),
        "unknown_rows": sum(table.rows for table in tables if table.kind == "unknown"),
        "malformed_relationship_rows": sum(
            table.malformed_rows for table in tables if table.kind == "relationship"
        ),
    }

    report: dict[str, Any] = {
        "input": str(dataset_dir),
        "summary": summary,
        "tables": [asdict(table) for table in tables],
        "relationships": [asdict(relationship) for relationship in relationships],
        "warnings": warnings,
    }
    if placement is not None and placement_statistics is not None:
        report["placementConfig"] = str(
            Path(placement_config).expanduser().resolve()
        )
        report["placement"] = build_placement_report(
            placement_statistics,
            routing_layout=placement.routing_layout(),
            placement_strategy=placement.strategy_name,
            dataset=f"LDBC SNB raw dataset ({dataset_dir.name})",
        )
    return report


def write_report_outputs(
    report: dict[str, Any],
    output_dir: str | Path,
    *,
    render_placement_graph: bool = True,
) -> dict[str, Any]:
    """Write the normal statistics JSON and optional placement artifacts."""

    target = Path(output_dir).expanduser().resolve()
    target.mkdir(parents=True, exist_ok=True)
    statistics_path = target / "ldbc-statistics.json"
    statistics_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    outputs: dict[str, Any] = {"statistics": str(statistics_path)}
    placement_report = report.get("placement")
    if placement_report is not None:
        outputs["placement"] = write_placement_outputs(
            target,
            placement_report,
            render_graph=render_placement_graph,
        )
    return outputs


class _FallbackPrettyTable:
    """Small dependency-free fallback matching the PrettyTable calls used here."""

    def __init__(self) -> None:
        self.title: str | None = None
        self.field_names: list[str] = []
        self.align: dict[str, str] = {}
        self.max_width: dict[str, int] = {}
        self._rows: list[list[str]] = []

    def add_row(self, row: Iterable[Any]) -> None:
        self._rows.append([str(value) for value in row])

    @staticmethod
    def _fit(value: str, width: int) -> str:
        if len(value) <= width:
            return value
        if width <= 1:
            return value[:width]
        return value[: width - 1] + "…"

    def __str__(self) -> str:
        headers = [str(value) for value in self.field_names]
        rows = [headers, *self._rows]
        widths: list[int] = []
        for index, header in enumerate(headers):
            width = max((len(row[index]) for row in rows if index < len(row)), default=len(header))
            width = min(width, self.max_width.get(header, width))
            widths.append(width)

        def border() -> str:
            return "+" + "+".join("-" * (width + 2) for width in widths) + "+"

        def render_row(row: list[str]) -> str:
            cells: list[str] = []
            for index, width in enumerate(widths):
                header = headers[index]
                value = self._fit(row[index] if index < len(row) else "", width)
                alignment = self.align.get(header, "c")
                if alignment == "l":
                    cells.append(f" {value:<{width}} ")
                elif alignment == "r":
                    cells.append(f" {value:>{width}} ")
                else:
                    cells.append(f" {value:^{width}} ")
            return "|" + "|".join(cells) + "|"

        lines: list[str] = []
        if self.title:
            lines.append(self.title)
        lines.extend([border(), render_row(headers), border()])
        lines.extend(render_row(row) for row in self._rows)
        lines.append(border())
        return "\n".join(lines)


def _pretty_table(title: str, fields: list[str]) -> Any:
    try:
        from prettytable import PrettyTable

        table: Any = PrettyTable()
    except ModuleNotFoundError:
        table = _FallbackPrettyTable()
    table.title = title
    table.field_names = fields
    return table


def _integer(value: int) -> str:
    return f"{value:,}"


def _decimal(value: float) -> str:
    return f"{value:,.3f}"


def _degree_row(name: str, node_type: str, values: dict[str, Any]) -> list[str]:
    return [
        name,
        node_type,
        _integer(values["population"]),
        _integer(values["active"]),
        _integer(values["total"]),
        _decimal(values["mean"]),
        _integer(values["minimum"]),
        _integer(values["median"]),
        _integer(values["p95"]),
        _integer(values["maximum"]),
        _decimal(values["stddev"]),
    ]


def print_report(report: dict[str, Any]) -> None:
    print(f"LDBC dataset: {report['input']}")
    print(
        "Degree populations include zero-degree nodes from the corresponding node table. "
        "Outgoing/incoming follow the raw first/second endpoint columns. Combined degree "
        "is incoming + outgoing; self-loops contribute two.\n"
    )

    summary_table = _pretty_table("Dataset summary", ["Metric", "Value"])
    summary_table.align["Metric"] = "l"
    summary_table.align["Value"] = "r"
    for key, value in report["summary"].items():
        summary_table.add_row([key.replace("_", " ").title(), _integer(value)])
    print(summary_table)

    rows_table = _pretty_table(
        "Logical CSV tables",
        ["Table", "Kind", "Section", "Files", "Rows", "From", "To", "Mode"],
    )
    for field in ("Table", "Kind", "Section", "From", "To", "Mode"):
        rows_table.align[field] = "l"
    rows_table.align["Files"] = "r"
    rows_table.align["Rows"] = "r"
    rows_table.max_width["Table"] = 42
    for table in report["tables"]:
        rows_table.add_row(
            [
                table["name"],
                table["kind"],
                ",".join(table["sections"]),
                _integer(table["files"]),
                _integer(table["rows"]),
                table["from_type"] or "-",
                table["to_type"] or "-",
                (
                    "directed"
                    if table["kind"] == "relationship"
                    and next(
                        item["directed"]
                        for item in report["relationships"]
                        if item["name"] == table["name"]
                    )
                    else "undirected"
                    if table["kind"] == "relationship"
                    else "-"
                ),
            ]
        )
    print(f"\n{rows_table}")

    fields = [
        "Relationship",
        "Node type",
        "Nodes",
        "Active",
        "Total",
        "Mean",
        "Min",
        "P50",
        "P95",
        "Max",
        "Stddev",
    ]
    outgoing_table = _pretty_table("Outgoing degree by relationship table", fields)
    incoming_table = _pretty_table("Incoming degree by relationship table", fields)
    combined_table = _pretty_table("Combined degree (incoming + outgoing)", fields)
    for table in (outgoing_table, incoming_table, combined_table):
        table.align["Relationship"] = "l"
        table.align["Node type"] = "l"
        table.max_width["Relationship"] = 42
        for field in ("Nodes", "Active", "Total", "Mean", "Min", "P50", "P95", "Max", "Stddev"):
            table.align[field] = "r"

    for relationship in report["relationships"]:
        outgoing_table.add_row(
            _degree_row(relationship["name"], relationship["from_type"], relationship["outgoing"])
        )
        incoming_table.add_row(
            _degree_row(relationship["name"], relationship["to_type"], relationship["incoming"])
        )
        combined_type = (
            relationship["from_type"]
            if relationship["from_type"] == relationship["to_type"]
            else f"{relationship['from_type']} + {relationship['to_type']}"
        )
        combined_table.add_row(
            _degree_row(relationship["name"], combined_type, relationship["combined"])
        )

    print(f"\n{outgoing_table}")
    print(f"\n{incoming_table}")
    print(f"\n{combined_table}")
    if report["warnings"]:
        print("\nWarnings:", file=sys.stderr)
        for warning in report["warnings"]:
            print(f"  - {warning}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Print row counts and per-relationship LDBC SNB degree statistics."
    )
    parser.add_argument(
        "--input-dir", required=True, help="Raw LDBC directory containing static/ and dynamic/."
    )
    parser.add_argument("--json-output", help="Optional machine-readable report path.")
    parser.add_argument(
        "--placement-config",
        help="Resolve the raw table counts through this placement.config.json.",
    )
    parser.add_argument(
        "--output-dir",
        help=(
            "Write ldbc-statistics.json and placement-report/graph artifacts here. "
            "When omitted with --placement-config, defaults to <dataset>/statistics."
        ),
    )
    parser.add_argument(
        "--storage-backends",
        nargs="+",
        choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"],
        default=list(DEFAULT_ROUTING_BACKENDS),
    )
    parser.add_argument(
        "--default-source",
        choices=["Neo4J", "ArangoDB", "PostgreSQL", "GPU"],
        default="Neo4J",
    )
    parser.add_argument(
        "--no-placement-graph-render",
        action="store_true",
        help="Generate JSON/TXT/DOT but skip Graphviz SVG/PDF rendering.",
    )
    args = parser.parse_args()

    report = analyze_dataset(
        args.input_dir,
        placement_config=args.placement_config,
        default_source=args.default_source,
        storage_backends=args.storage_backends,
    )
    if args.json_output:
        output = Path(args.json_output).expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    outputs: dict[str, Any] | None = None
    if args.output_dir or args.placement_config:
        output_dir = (
            Path(args.output_dir).expanduser().resolve()
            if args.output_dir
            else Path(report["input"]) / "statistics"
        )
        outputs = write_report_outputs(
            report,
            output_dir,
            render_placement_graph=not args.no_placement_graph_render,
        )

    print_report(report)
    if outputs is not None:
        print("\nGenerated statistics artifacts:")
        print(json.dumps(outputs, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
