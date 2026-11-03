"""Content locking and fixed queries for the independent initial-CSV oracle."""

from __future__ import annotations

from collections import Counter
import csv
import hashlib
import heapq
import json
from pathlib import Path
import re

from .bfs import project
from .ldbc_csv import load_csv_graph
from .model import Graph
from .schema import AUX_FILES, EDGE_FILES, NODE_FILES, NODE_TYPES


LOCK_FORMAT = "polygraph.reference.dataset-lock"
QUERY_FORMAT = "polygraph.reference.queries"
VERSION = 1


def read_json(path: Path) -> dict:
    def unique_pairs(pairs: list[tuple[str, object]]) -> dict:
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"Duplicate JSON key {key!r} in {path}")
            result[key] = value
        return result

    def invalid_constant(value: str) -> None:
        raise ValueError(f"Non-finite JSON number {value!r} in {path}")

    value = json.loads(Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique_pairs,
                       parse_constant=invalid_constant)
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def write_json(path: Path, value: dict) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def lock_digest(lock: dict) -> str:
    encoded = json.dumps(lock, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _inventory(root: Path) -> list[dict]:
    paths = [path for area in ("static", "dynamic") for path in (root / area).rglob("*.csv")]
    if (root / "params.ini").exists():
        paths.append(root / "params.ini")
    records = []
    for path in sorted(paths, key=lambda p: p.relative_to(root).as_posix()):
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root):
            raise ValueError(f"Snapshot inventory is not a regular in-root file: {path}")
        entry = {"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size,
                 "sha256": file_digest(path)}
        if path.suffix == ".csv":
            with path.open(encoding="utf-8", newline="") as stream:
                rows = csv.reader(stream, delimiter="|", strict=True)
                entry["header"] = next(rows, [])
                entry["rows"] = sum(1 for _ in rows)
        records.append(entry)
    return records


def _schema_digest() -> str:
    return lock_digest({"nodes": NODE_FILES, "edges": EDGE_FILES, "attributes": AUX_FILES})


def create_lock(root: Path, *, require_complete: bool = True) -> dict:
    """Explicitly enroll this snapshot; ordinary verification never updates it.

    A complete lock claims the v1 SF0.1 *initial* CsvBasic/LongDateFormatter
    profile only when params.ini declares it. Upstream commit/seed/archive
    provenance cannot be inferred from raw CSV and is explicitly unknown.
    """
    root = Path(root).resolve()
    graph = load_csv_graph(root, require_complete=require_complete)
    parameters = {}
    if (root / "params.ini").is_file():
        for line in (root / "params.ini").read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.lstrip().startswith("#"):
                key, separator, value = line.partition(":")
                if not separator or key.strip() in parameters:
                    raise ValueError("Invalid or duplicate generator parameter")
                parameters[key.strip()] = value.strip()
    if require_complete:
        if parameters.get("ldbc.snb.datagen.generator.scaleFactor") != "snb.interactive.0.1":
            raise ValueError("Complete SF0.1 lock requires matching scaleFactor in params.ini")
        serializers = ("dynamicActivitySerializer", "dynamicPersonSerializer", "staticSerializer")
        if not all("CsvBasic" in parameters.get("ldbc.snb.datagen.serializer." + name, "")
                   for name in serializers):
            raise ValueError("Complete SF0.1 lock requires CsvBasic serializers in params.ini")
        if not parameters.get("ldbc.snb.datagen.serializer.dateFormatter", "").endswith(".LongDateFormatter"):
            raise ValueError("Complete SF0.1 lock requires LongDateFormatter in params.ini")
    files = _inventory(root)
    return {
        "format": LOCK_FORMAT,
        "version": VERSION,
        "dataset": "ldbc-snb-interactive-v1-initial" if require_complete else "authored-partial-csv",
        "scale_factor": "0.1" if require_complete else None,
        "serializer": "CsvBasic",
        "identity": ["entity_type", "original_ldbc_id"],
        "complete_families": require_complete,
        "schema_sha256": _schema_digest(),
        "snapshot_sha256": lock_digest({"files": files}),
        "provenance": {
            "status": "local snapshot; generator commit, seed and archive not independently established",
            "generator_repository": "https://github.com/ldbc/ldbc_snb_datagen_hadoop",
            "generator_commit": None,
            "generator_seed": None,
            "archive_sha256": None,
            "parameters": parameters,
        },
        "excluded": ["root updateStream*.csv", "Hadoop .crc sidecars", "non-CSV runtime artifacts"],
        "counts": {
            "vertices": len(graph.vertices),
            "relationships": len(graph.edges),
            "vertices_by_type": dict(sorted(Counter(v[0] for v in graph.vertices).items())),
            "relationships_by_type": dict(sorted(Counter(e.relation for e in graph.edges).items())),
        },
        "files": files,
    }


def verify_lock(root: Path, lock: dict) -> Graph:
    """Fail closed on different bytes, shards, schema, counts or lock metadata."""
    root = Path(root).resolve()
    if (lock.get("format") != LOCK_FORMAT or type(lock.get("version")) is not int
            or lock["version"] != VERSION or type(lock.get("complete_families")) is not bool):
        raise ValueError("Unsupported dataset lock")
    if lock.get("schema_sha256") != _schema_digest() or lock.get("files") != _inventory(root):
        raise ValueError("Dataset lock checksum/inventory/schema mismatch; do not regenerate it to hide drift")
    # Re-enrollment validates topology and derives counts independently of claims
    # in the supplied lock. It is in-memory only and never writes the lock file.
    if lock_digest(lock) != lock_digest(create_lock(root, require_complete=lock["complete_families"])):
        raise ValueError("Dataset lock metadata or counts mismatch")
    return load_csv_graph(root, require_complete=lock["complete_families"])


def generate_queries(graph: Graph, lock: dict) -> dict:
    """Select up to 24 fixed roots, then emit explicit deterministic queries."""
    if not graph.vertices:
        raise ValueError("Cannot select roots from an empty graph")
    projection = project(graph)
    roots = []
    selected = set()

    def add_root(name: str, vertex: tuple[str, int]) -> None:
        if vertex not in selected:
            selected.add(vertex)
            roots.append({"name": name, "vertex_type": vertex[0], "vertex_id": str(vertex[1])})

    for node_type in NODE_TYPES:
        vertices = sorted(v for v in graph.vertices if v[0] == node_type)
        if vertices:
            add_root(node_type.lower() + "-min", vertices[0])
            highest = min(vertices, key=lambda v: (-len(projection.adjacency[v]), v))
            add_root(node_type.lower() + "-max-degree", highest)
    remaining = heapq.nsmallest(
        min(24, len(graph.vertices)) - len(roots),
        (v for v in graph.vertices if v not in selected),
        key=lambda v: (hashlib.sha256(f"{v[0]}:{v[1]}".encode("ascii")).digest(), v),
    )
    for index, vertex in enumerate(remaining):
        add_root(f"hash-rank-{index:02}", vertex)
    queries = []

    def add_query(name: str, root: dict, depth: int | None, *, direction: str = "both",
                  edge_types: tuple[str, ...] = (), target_types: tuple[str, ...] = (), parents: bool = True) -> None:
        queries.append({"name": name, "root": root["name"], "direction": direction,
                        "max_depth": depth, "edge_types": list(edge_types),
                        "target_types": list(target_types), "return_predecessors": parents})

    for root in roots:
        add_query(root["name"] + "-both-d7", root, 7)
    primary = next((r for r in roots if r["vertex_type"] == "Person"), roots[0])
    for depth in (0, 1, 2, 3, None):
        add_query(f"boundary-d{depth if depth is not None else 'unlimited'}", primary, depth)
    for node_type in NODE_TYPES:
        root = next((r for r in roots if r["vertex_type"] == node_type), None)
        if root:
            add_query(node_type.lower() + "-outgoing-d3", root, 3, direction="outgoing")
    for root in [r for r in roots if r["vertex_type"] == "Person"][:3]:
        add_query(root["name"] + "-knows-unlimited", root, None, edge_types=("PERSON_KNOWS_PERSON",))
    add_query("target-person-d3", primary, 3, target_types=("Person",))
    add_query("distance-only-d3", primary, 3, parents=False)
    return {"format": QUERY_FORMAT, "version": VERSION, "dataset_lock_sha256": lock_digest(lock),
            "selection": "per-type min ID + max distinct Both degree (ties min ID), fill to 24 by SHA256(type:id)",
            "roots": roots, "queries": queries}


def validate_queries(manifest: dict, lock: dict, graph: Graph) -> list[tuple[dict, tuple[str, int]]]:
    if (manifest.get("format") != QUERY_FORMAT or type(manifest.get("version")) is not int
            or manifest["version"] != VERSION or manifest.get("dataset_lock_sha256") != lock_digest(lock)):
        raise ValueError("Query manifest format/version/dataset-lock mismatch")
    roots = manifest.get("roots")
    queries = manifest.get("queries")
    if not isinstance(roots, list) or not roots or not isinstance(queries, list) or not queries:
        raise ValueError("Query manifest must contain nonempty roots and queries")
    safe_name = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]*\Z")
    root_by_name = {}
    seen_vertices = set()
    for root in roots:
        if not isinstance(root, dict) or set(root) != {"name", "vertex_type", "vertex_id"}:
            raise ValueError("Malformed root record")
        name = root["name"]
        raw_id = root["vertex_id"]
        if (not isinstance(name, str) or not safe_name.fullmatch(name) or name in root_by_name
                or not isinstance(raw_id, str) or not re.fullmatch(r"0|[1-9][0-9]*", raw_id)):
            raise ValueError("Invalid/duplicate root name or noncanonical raw ID")
        vertex = (root["vertex_type"], int(raw_id))
        if vertex not in graph.vertices or vertex in seen_vertices:
            raise ValueError("Root is missing from graph or repeated")
        root_by_name[name] = vertex
        seen_vertices.add(vertex)
    names = set()
    cases = []
    fields = {"name", "root", "direction", "max_depth", "edge_types", "target_types", "return_predecessors"}
    for query in queries:
        if not isinstance(query, dict) or set(query) != fields:
            raise ValueError("Malformed query record")
        name = query["name"]
        if not isinstance(name, str) or not safe_name.fullmatch(name) or name in names:
            raise ValueError("Invalid/duplicate query name")
        names.add(name)
        if not isinstance(query["root"], str) or query["root"] not in root_by_name:
            raise ValueError("Query names an undefined root")
        if query["max_depth"] is not None and (type(query["max_depth"]) is not int or query["max_depth"] < 0):
            raise ValueError("Query depth must be a nonnegative integer or null")
        if query["direction"] not in ("both", "outgoing") or type(query["return_predecessors"]) is not bool:
            raise ValueError("Invalid query direction/predecessor flag")
        for field in ("edge_types", "target_types"):
            allowed = NODE_TYPES if field == "target_types" else tuple(v[1] for v in EDGE_FILES.values())
            value = query[field]
            if (not isinstance(value, list) or any(not isinstance(v, str) or v not in allowed for v in value)
                    or len(value) != len(set(value))):
                raise ValueError("Invalid query filter")
        cases.append((query, root_by_name[query["root"]]))
    return cases
