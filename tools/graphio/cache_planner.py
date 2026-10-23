from __future__ import annotations

import array
import itertools
import json
import math
import sqlite3
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator

from .id_codec import (
    LOCATION_MMAP,
    LOCATION_REDIS,
    BackendLayout,
    canonical_global_id,
    gpu_location_bit,
)
from .reader import open_graph_reader

INDEX_RECORD = struct.Struct("<QQQ")  # canonical source id, neighbor offset, neighbor count
UINT64 = struct.Struct("<Q")
UINT32 = struct.Struct("<I")


@dataclass(frozen=True)
class CacheArtifact:
    name: str
    location_mask: int
    index_path: Path
    neighbors_path: Path
    vertices: int
    relations: int
    bytes: int


@dataclass(frozen=True)
class GpuProjectionArtifact:
    device_id: int
    manifest_path: Path
    global_ids_path: Path
    sources_path: Path
    destinations_path: Path
    resident_vertices: int
    vertices: int
    adjacency_pairs: int
    bytes: int


def _assert_binary_array_layout() -> None:
    if array.array("Q").itemsize != UINT64.size or array.array("I").itemsize != UINT32.size:
        raise RuntimeError("This platform does not provide 64-bit Q / 32-bit I array elements")


def load_cache_config(path: str | Path | None) -> dict[str, Any]:
    if path is None:
        return {"version": 1, "gpus": [], "mmap": {"enabled": False}, "redis": {"enabled": False}}
    source = Path(path).expanduser().resolve()
    raw = json.loads(source.read_text(encoding="utf-8"))
    if int(raw.get("version", 1)) != 1:
        raise ValueError(f"Unsupported cache configuration version in {source}")
    return raw


def _bytes(value: Any, default: int = 0) -> int:
    if value is None:
        return default
    if isinstance(value, (int, float)):
        return int(value)
    text = str(value).strip().lower().replace(" ", "")
    multipliers = {
        "kb": 1000, "kib": 1024,
        "mb": 1000**2, "mib": 1024**2,
        "gb": 1000**3, "gib": 1024**3,
        "tb": 1000**4, "tib": 1024**4,
        "b": 1,
    }
    for suffix in sorted(multipliers, key=len, reverse=True):
        if text.endswith(suffix):
            return int(float(text[:-len(suffix)]) * multipliers[suffix])
    return int(text)


def _cache_location_mask(values: Iterable[str] | None) -> int:
    mask = 0
    for raw in values or ():
        value = str(raw).strip()
        normalized = value.lower()
        if normalized == "redis":
            mask |= LOCATION_REDIS
        elif normalized in {"mmap", "memorymapped", "memory-mapped"}:
            mask |= LOCATION_MMAP
        elif normalized.startswith("gpu:"):
            try:
                device_id = int(value.split(":", 1)[1])
            except ValueError as exc:
                raise ValueError(f"Invalid GPU cache location {value!r}") from exc
            mask |= gpu_location_bit(device_id)
        else:
            raise ValueError(
                f"Unsupported cache location {value!r}; expected Redis, MMap, or GPU:<device>"
            )
    return mask


def _configure_planning_connection(connection: sqlite3.Connection) -> None:
    connection.execute("PRAGMA journal_mode=OFF")
    connection.execute("PRAGMA synchronous=OFF")
    connection.execute("PRAGMA temp_store=FILE")
    connection.execute("PRAGMA cache_size=-131072")  # ~128 MiB


def _open_planning_db(path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(path)
    _configure_planning_connection(connection)
    connection.executescript(
        """
        CREATE TABLE nodes(
            id INTEGER PRIMARY KEY,
            type TEXT NOT NULL,
            source TEXT NOT NULL,
            degree INTEGER NOT NULL DEFAULT 0,
            cache_mask INTEGER NOT NULL DEFAULT 0
        );
        -- Keep one physical row per graph relationship.  The old planner
        -- materialized both directions here and then built two indexes, which
        -- roughly doubled the dominant SF-scale planning-table footprint.
        -- Undirected cache adjacency is derived logically in the selection and
        -- artifact queries below.
        CREATE TABLE edges(src INTEGER NOT NULL, dst INTEGER NOT NULL);
        CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
        """
    )
    return connection


def _graph_signature(graph_path: Path) -> dict[str, str]:
    stat = graph_path.stat()
    return {
        "graph.path": str(graph_path.resolve()),
        "graph.size": str(stat.st_size),
        "graph.mtime_ns": str(stat.st_mtime_ns),
    }


def _write_graph_signature(db: sqlite3.Connection, graph_path: Path) -> None:
    db.executemany(
        "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
        _graph_signature(graph_path).items(),
    )
    db.commit()


def _open_existing_planning_db(path: Path, graph_path: Path) -> sqlite3.Connection:
    if not path.is_file():
        raise FileNotFoundError(f"Cache planning database does not exist: {path}")
    connection = sqlite3.connect(path)
    _configure_planning_connection(connection)
    tables = {
        str(row[0])
        for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name IN ('nodes','edges','meta')"
        )
    }
    if tables != {"nodes", "edges", "meta"}:
        connection.close()
        raise ValueError(
            f"Cache planning database {path} is from an older/incomplete planner; rebuilding it"
        )
    indexes = {
        str(row[0])
        for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='index' "
            "AND name IN ('edges_src_idx','edges_dst_idx')"
        )
    }
    if indexes != {"edges_src_idx", "edges_dst_idx"}:
        connection.close()
        raise ValueError(
            f"Cache planning database {path} did not finish indexing; "
            "resume requires edges_src_idx and edges_dst_idx"
        )
    stored = dict(connection.execute("SELECT key,value FROM meta WHERE key LIKE 'graph.%'"))
    expected = _graph_signature(graph_path)
    if any(stored.get(key) != value for key, value in expected.items()):
        connection.close()
        raise ValueError(
            f"Cache planning database {path} belongs to a different graph revision; rebuilding it"
        )
    return connection


def _planning_db_counts(db: sqlite3.Connection) -> tuple[int, int]:
    # nodes is a WITHOUT-extra-mapping-style INTEGER PRIMARY KEY table, so COUNT
    # is the only reliable exact cardinality.  For edges we only ever append and
    # never delete, therefore max(rowid) is the exact count and is index-cheap.
    node_count = int(db.execute("SELECT count(*) FROM nodes").fetchone()[0])
    relationship_count = int(
        db.execute("SELECT COALESCE(max(rowid), 0) FROM edges").fetchone()[0]
    )
    return node_count, relationship_count


def _signed_sqlite_id(value: int) -> int:
    # sqlite INTEGER is signed int64. Generated location-mask ids can set bit 63.
    return value - (1 << 64) if value & (1 << 63) else value


def _unsigned_sqlite_id(value: int) -> int:
    return value & ((1 << 64) - 1)


def _index_graph(graph_path: Path, db: sqlite3.Connection) -> tuple[int, int]:
    reader = open_graph_reader(graph_path)
    node_batch: list[tuple[int, str, str]] = []
    node_count = 0
    started = time.monotonic()
    last_report = started
    for node in reader.iter_nodes():
        node_batch.append((_signed_sqlite_id(canonical_global_id(node.uuid)), node.type, node.source))
        node_count += 1
        if len(node_batch) >= 50_000:
            db.executemany("INSERT INTO nodes(id,type,source) VALUES(?,?,?)", node_batch)
            node_batch.clear()
        now = time.monotonic()
        if now - last_report >= 5.0:
            print(
                f"[cache-plan] indexed {node_count:,} nodes ({now - started:,.1f}s)",
                file=sys.stderr,
                flush=True,
            )
            last_report = now
    if node_batch:
        db.executemany("INSERT INTO nodes(id,type,source) VALUES(?,?,?)", node_batch)
    db.commit()

    edge_batch: list[tuple[int, int]] = []
    relationship_count = 0
    for rel in reader.iter_relationships():
        source = _signed_sqlite_id(canonical_global_id(rel.from_uuid))
        target = _signed_sqlite_id(canonical_global_id(rel.to_uuid))
        edge_batch.append((source, target))
        relationship_count += 1
        if len(edge_batch) >= 100_000:
            db.executemany("INSERT INTO edges(src,dst) VALUES(?,?)", edge_batch)
            edge_batch.clear()
        now = time.monotonic()
        if now - last_report >= 5.0:
            print(
                f"[cache-plan] indexed {node_count:,} nodes and {relationship_count:,} relationships "
                f"({now - started:,.1f}s)",
                file=sys.stderr,
                flush=True,
            )
            last_report = now
    if edge_batch:
        db.executemany("INSERT INTO edges(src,dst) VALUES(?,?)", edge_batch)
    db.commit()
    print("[cache-plan] building edge indexes and degree table", file=sys.stderr, flush=True)
    db.execute("CREATE INDEX edges_src_idx ON edges(src)")
    db.execute("CREATE INDEX edges_dst_idx ON edges(dst)")
    # Preserve the previous cache-planner semantics: every non-self edge
    # contributes one neighbor to each endpoint, while a self-loop contributes
    # one.  Both lookups are index-backed and keep RAM bounded.
    db.execute(
        """
        UPDATE nodes
        SET degree =
            (SELECT count(*) FROM edges WHERE edges.src=nodes.id) +
            (SELECT count(*) FROM edges WHERE edges.dst=nodes.id) -
            (SELECT count(*) FROM edges WHERE edges.src=nodes.id AND edges.dst=nodes.id)
        """
    )
    db.commit()
    _write_graph_signature(db, graph_path)
    return node_count, relationship_count


def _estimated_vertex_bytes(
    degree: int, fixed_overhead_bytes: int = 24, neighbor_bytes: int = 8
) -> int:
    # The generic cache representation stores one uint64 neighbor (8 bytes). GPU
    # planning deliberately uses a larger per-adjacency estimate because cuGraph
    # construction temporarily owns dense COO input, CSR output and workspace at
    # the same time. This prevents a nominal "6GiB" cache plan from being one
    # that can only fail later during runtime construction.
    return max(0, int(fixed_overhead_bytes)) + max(0, int(degree)) * max(1, int(neighbor_bytes))


def _select_policy(
    db: sqlite3.Connection,
    *,
    policy: dict[str, Any],
    location_bit: int,
    memory_budget: int = 0,
    relation_budget: int = 0,
    vertex_budget: int = 0,
    allowed_types: Iterable[str] | None = None,
    allowed_sources: Iterable[str] | None = None,
    excluded_sources: Iterable[str] | None = None,
    fixed_overhead_bytes: int = 24,
    neighbor_bytes: int = 8,
    require_mask: int = 0,
    exclude_mask: int = 0,
) -> dict[str, Any]:
    kind = str(policy.get("kind", policy.get("type", "top-degree"))).lower().replace("_", "-")
    include_neighbors = bool(policy.get("include_neighbors", policy.get("includeNeighbors", False)))
    requested_vertices = int(policy.get("vertex_count", policy.get("vertices", policy.get("count", vertex_budget or 0))) or 0)
    vertex_fraction_raw = policy.get("vertex_fraction", policy.get("vertexFraction"))
    rank_start_fraction_raw = policy.get("rank_start_fraction", policy.get("rankStartFraction", 0.0))
    rank_start_count = int(policy.get("rank_start_count", policy.get("rankStartCount", 0)) or 0)
    min_degree_raw = policy.get("min_degree", policy.get("minDegree"))
    max_degree_raw = policy.get("max_degree", policy.get("maxDegree"))
    min_degree = None if min_degree_raw is None else int(min_degree_raw)
    max_degree = None if max_degree_raw is None else int(max_degree_raw)
    if min_degree is not None and min_degree < 0:
        raise ValueError("cache policy min_degree must be non-negative")
    if max_degree is not None and max_degree < 0:
        raise ValueError("cache policy max_degree must be non-negative")
    if (
        min_degree is not None
        and max_degree is not None
        and min_degree > max_degree
    ):
        raise ValueError("cache policy min_degree cannot exceed max_degree")
    if requested_vertices and vertex_fraction_raw is not None:
        raise ValueError("cache policy cannot define both vertex_count and vertex_fraction")
    vertex_fraction = None if vertex_fraction_raw is None else float(vertex_fraction_raw)
    rank_start_fraction = float(rank_start_fraction_raw or 0.0)
    if vertex_fraction is not None and not 0.0 <= vertex_fraction <= 1.0:
        raise ValueError("cache policy vertex_fraction must be in [0, 1]")
    if not 0.0 <= rank_start_fraction <= 1.0:
        raise ValueError("cache policy rank_start_fraction must be in [0, 1]")
    if rank_start_count < 0:
        raise ValueError("cache policy rank_start_count must be non-negative")
    if rank_start_count and rank_start_fraction:
        raise ValueError("cache policy cannot define both rank_start_count and rank_start_fraction")
    if vertex_budget and (requested_vertices == 0 or requested_vertices > vertex_budget):
        requested_vertices = vertex_budget
    policy_memory = _bytes(policy.get("memory_limit_bytes", policy.get("memory", 0)))
    if policy_memory and (memory_budget == 0 or policy_memory < memory_budget):
        memory_budget = policy_memory
    policy_relations = int(policy.get("relation_limit", policy.get("relations", 0)) or 0)
    if policy_relations and (relation_budget == 0 or policy_relations < relation_budget):
        relation_budget = policy_relations

    base_where: list[str] = []
    base_params: list[Any] = []
    types = tuple(str(value) for value in (allowed_types or policy.get("node_types", policy.get("nodeTypes", []))))
    if types:
        base_where.append("type IN (%s)" % ",".join("?" for _ in types))
        base_params.extend(types)
    owner_sources = tuple(str(value) for value in policy.get("owner_sources", policy.get("ownerSources", [])))
    if owner_sources:
        base_where.append("source IN (%s)" % ",".join("?" for _ in owner_sources))
        base_params.extend(owner_sources)
    allowed_sources_tuple = tuple(str(value) for value in (allowed_sources or ()))
    excluded_sources_tuple = tuple(str(value) for value in (excluded_sources or ()))
    if allowed_sources_tuple:
        base_where.append("source IN (%s)" % ",".join("?" for _ in allowed_sources_tuple))
        base_params.extend(allowed_sources_tuple)
    if excluded_sources_tuple:
        base_where.append("source NOT IN (%s)" % ",".join("?" for _ in excluded_sources_tuple))
        base_params.extend(excluded_sources_tuple)
    if min_degree is not None:
        base_where.append("degree >= ?")
        base_params.append(min_degree)
    if max_degree is not None:
        base_where.append("degree <= ?")
        base_params.append(max_degree)

    base_clause = "WHERE " + " AND ".join(base_where) if base_where else ""
    base_candidate_count = int(
        db.execute(f"SELECT count(*) FROM nodes {base_clause}", base_params).fetchone()[0]
    )
    if vertex_fraction is not None:
        requested_vertices = int(math.ceil(base_candidate_count * vertex_fraction))
        if vertex_budget and requested_vertices > vertex_budget:
            requested_vertices = vertex_budget
    rank_skip = rank_start_count or int(math.floor(base_candidate_count * rank_start_fraction))

    where = list(base_where)
    params = list(base_params)
    if require_mask:
        where.append("(cache_mask & ?) != 0")
        params.append(require_mask)
    if exclude_mask:
        where.append("(cache_mask & ?) = 0")
        params.append(exclude_mask)
    clause = "WHERE " + " AND ".join(where) if where else ""

    if kind in {"all", "mapped", "node-types", "types"}:
        order = "id"
    elif kind in {"top-degree", "best", "top-degree-with-neighbors"}:
        order = "degree DESC, id"
        if kind == "top-degree-with-neighbors":
            include_neighbors = True
    else:
        raise ValueError(f"Unsupported cache policy kind: {kind}")

    db.execute("DROP TABLE IF EXISTS selected_seed")
    db.execute("CREATE TEMP TABLE selected_seed(id INTEGER PRIMARY KEY)")
    used_bytes = used_relations = selected = 0
    query = f"SELECT id, degree FROM nodes {clause} ORDER BY {order}"
    seed_rows: list[tuple[int]] = []
    skipped = 0
    for node_id, degree in db.execute(query, params):
        if skipped < rank_skip:
            skipped += 1
            continue
        cost = _estimated_vertex_bytes(degree, fixed_overhead_bytes, neighbor_bytes)
        if requested_vertices and selected >= requested_vertices:
            break
        if memory_budget and used_bytes + cost > memory_budget:
            continue
        if relation_budget and used_relations + degree > relation_budget:
            continue
        seed_rows.append((node_id,))
        used_bytes += cost
        used_relations += int(degree)
        selected += 1
        if len(seed_rows) >= 20_000:
            db.executemany("INSERT OR IGNORE INTO selected_seed(id) VALUES(?)", seed_rows)
            seed_rows.clear()
    if seed_rows:
        db.executemany("INSERT OR IGNORE INTO selected_seed(id) VALUES(?)", seed_rows)
    db.commit()

    db.execute(
        "UPDATE nodes SET cache_mask=(cache_mask | ?) WHERE id IN (SELECT id FROM selected_seed)",
        (location_bit,),
    )
    db.commit()

    if include_neighbors:
        # Neighbor promotion is intentionally budget-aware: highest-degree neighbors
        # are admitted first because they avoid the most future backing-store work.
        neighbor_filters: list[str] = ["already.id IS NULL"]
        neighbor_params: list[Any] = []
        if require_mask:
            neighbor_filters.append("(n.cache_mask & ?) != 0")
            neighbor_params.append(require_mask)
        if exclude_mask:
            neighbor_filters.append("(n.cache_mask & ?) = 0")
            neighbor_params.append(exclude_mask)
        if allowed_sources_tuple:
            neighbor_filters.append("n.source IN (%s)" % ",".join("?" for _ in allowed_sources_tuple))
            neighbor_params.extend(allowed_sources_tuple)
        if excluded_sources_tuple:
            neighbor_filters.append("n.source NOT IN (%s)" % ",".join("?" for _ in excluded_sources_tuple))
            neighbor_params.extend(excluded_sources_tuple)
        if min_degree is not None:
            neighbor_filters.append("n.degree >= ?")
            neighbor_params.append(min_degree)
        if max_degree is not None:
            neighbor_filters.append("n.degree <= ?")
            neighbor_params.append(max_degree)
        neighbor_query = f"""
            SELECT n.id, n.degree
            FROM nodes n
            JOIN (
                SELECT e.dst AS id
                FROM edges e JOIN selected_seed s ON s.id=e.src
                UNION
                SELECT e.src AS id
                FROM edges e JOIN selected_seed s ON s.id=e.dst
            ) candidate ON candidate.id=n.id
            LEFT JOIN selected_seed already ON already.id=n.id
            WHERE {' AND '.join(neighbor_filters)}
            ORDER BY n.degree DESC, n.id
        """
        neighbor_rows: list[tuple[int]] = []
        for node_id, degree in db.execute(neighbor_query, neighbor_params):
            cost = _estimated_vertex_bytes(degree, fixed_overhead_bytes, neighbor_bytes)
            if vertex_budget and selected >= vertex_budget:
                break
            if memory_budget and used_bytes + cost > memory_budget:
                continue
            if relation_budget and used_relations + degree > relation_budget:
                continue
            neighbor_rows.append((location_bit, node_id))
            used_bytes += cost
            used_relations += int(degree)
            selected += 1
            if len(neighbor_rows) >= 20_000:
                db.executemany("UPDATE nodes SET cache_mask=(cache_mask | ?) WHERE id=?", neighbor_rows)
                neighbor_rows.clear()
        if neighbor_rows:
            db.executemany("UPDATE nodes SET cache_mask=(cache_mask | ?) WHERE id=?", neighbor_rows)
        db.commit()

    return {
        "vertices": selected,
        "estimatedRelations": used_relations,
        "estimatedBytes": used_bytes,
        "candidateVertices": base_candidate_count,
        "rankSkippedVertices": min(rank_skip, base_candidate_count),
        "requestedVertexFraction": vertex_fraction,
        "minDegree": min_degree,
        "maxDegree": max_degree,
    }


def _build_binary_artifact(db: sqlite3.Connection, output_dir: Path, name: str, location_bit: int) -> CacheArtifact:
    output_dir.mkdir(parents=True, exist_ok=True)
    index_path = output_dir / "index.bin"
    neighbors_path = output_dir / "neighbors.bin"
    vertex_count = relation_count = 0
    offset = 0

    # SQLite INTEGER is signed while PolyGraph IDs are uint64.  The explicit
    # two-part ordering below is uint64 order: non-negative signed values first,
    # then negative signed values.  This keeps index.bin suitable for a normal
    # uint64 binary search even if a future backend uses bit 63.
    sources = db.execute(
        """
        SELECT id
        FROM nodes
        WHERE (cache_mask & ?) != 0
        ORDER BY (id < 0), id
        """,
        (location_bit,),
    )
    adjacency = iter(db.execute(
        """
        SELECT oriented.src, oriented.dst
        FROM (
            SELECT e.src AS src, e.dst AS dst
            FROM edges e
            JOIN nodes s ON s.id=e.src
            WHERE (s.cache_mask & ?) != 0
            UNION ALL
            SELECT e.dst AS src, e.src AS dst
            FROM edges e
            JOIN nodes s ON s.id=e.dst
            WHERE e.src != e.dst AND (s.cache_mask & ?) != 0
        ) AS oriented
        ORDER BY (oriented.src < 0), oriented.src
        """,
        (location_bit, location_bit),
    ))
    current = next(adjacency, None)
    with index_path.open("wb") as index_handle, neighbors_path.open("wb") as neighbor_handle:
        for (source_signed,) in sources:
            count = 0
            source_id = canonical_global_id(_unsigned_sqlite_id(source_signed))
            while current is not None and current[0] == source_signed:
                _, dst_signed = current
                destination = canonical_global_id(_unsigned_sqlite_id(dst_signed))
                neighbor_handle.write(UINT64.pack(destination))
                count += 1
                relation_count += 1
                current = next(adjacency, None)
            index_handle.write(INDEX_RECORD.pack(source_id, offset, count))
            offset += count
            vertex_count += 1

    total_bytes = index_path.stat().st_size + neighbors_path.stat().st_size
    manifest = {
        "format": "polygraph.cache.csr",
        "version": 1,
        "name": name,
        "locationMask": location_bit,
        "vertices": vertex_count,
        "relations": relation_count,
        "bytes": total_bytes,
        "indexRecord": "<uint64 canonical_id,uint64 neighbor_offset,uint64 neighbor_count>",
        "neighborRecord": "<uint64 canonical_global_id>",
        "index": index_path.name,
        "neighbors": neighbors_path.name,
    }
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return CacheArtifact(name, location_bit, index_path, neighbors_path, vertex_count, relation_count, total_bytes)


def _write_u64_column(path: Path, rows: Iterator[tuple[int]], *, label: str) -> int:
    count = 0
    started = time.monotonic()
    last_report = started
    with path.open("wb") as handle:
        while True:
            chunk = list(itertools.islice(rows, 200_000))
            if not chunk:
                break
            values = array.array("Q", (_unsigned_sqlite_id(int(row[0])) for row in chunk))
            if sys.byteorder != "little":
                values.byteswap()
            values.tofile(handle)
            count += len(values)
            now = time.monotonic()
            if now - last_report >= 5.0:
                print(f"[cache-plan] {label}: wrote {count:,} records", file=sys.stderr, flush=True)
                last_report = now
    return count


def _build_gpu_projection_artifact(
    db: sqlite3.Connection, output_dir: Path, device_id: int, location_bit: int
) -> GpuProjectionArtifact:
    """Materialize the exact compact GPU projection once during cache generation.

    The runtime used to rescan graph.jsonl, collect GlobalIds, renumber every edge,
    mirror, sort and deduplicate on every process start.  This artifact moves all
    graph-dependent work into cache construction.  The stored edge list is already
    dense, directed and deduplicated.  Each GPU-resident source contains both its
    incoming and outgoing logical neighbors, so runtime does not need to mirror it.
    """
    _assert_binary_array_layout()
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = output_dir / "manifest.json"
    global_ids_path = output_dir / "global_ids.bin"
    sources_path = output_dir / "sources.bin"
    destinations_path = output_dir / "destinations.bin"

    print(
        f"[cache-plan] GPU:{device_id}: materializing reusable dense projection",
        file=sys.stderr,
        flush=True,
    )

    # Keep the temporary structures inside SQLite so even very large projections
    # do not require a Python dictionary containing millions of GlobalIds.
    db.executescript(
        """
        DROP TABLE IF EXISTS temp.gpu_resident_vertices;
        DROP TABLE IF EXISTS temp.gpu_projection_vertices;
        DROP TABLE IF EXISTS temp.gpu_dense_ids;
        CREATE TEMP TABLE gpu_resident_vertices(id INTEGER PRIMARY KEY);
        CREATE TEMP TABLE gpu_projection_vertices(id INTEGER PRIMARY KEY);
        CREATE TEMP TABLE gpu_dense_ids(
            dense INTEGER PRIMARY KEY,
            id INTEGER NOT NULL UNIQUE
        );
        """
    )
    db.execute(
        "INSERT INTO gpu_resident_vertices(id) "
        "SELECT id FROM nodes WHERE (cache_mask & ?) != 0",
        (location_bit,),
    )
    db.execute(
        "INSERT INTO gpu_projection_vertices(id) SELECT id FROM gpu_resident_vertices"
    )
    # Add proxy/destination IDs needed to return neighbors from the GPU.  They are
    # projection vertices only; residency is still defined solely by cache_mask.
    db.execute(
        """
        INSERT OR IGNORE INTO gpu_projection_vertices(id)
        SELECT e.dst
        FROM edges e JOIN gpu_resident_vertices s ON s.id=e.src
        """
    )
    db.execute(
        """
        INSERT OR IGNORE INTO gpu_projection_vertices(id)
        SELECT e.src
        FROM edges e JOIN gpu_resident_vertices s ON s.id=e.dst
        """
    )
    # INTEGER PRIMARY KEY is assigned in insertion order.  Sorting by (id < 0),id
    # is uint64 ordering for the signed SQLite representation.  dense-1 is thus
    # exactly the index DenseIdMapper will use at runtime.
    db.execute(
        """
        INSERT INTO gpu_dense_ids(id)
        SELECT id FROM gpu_projection_vertices ORDER BY (id < 0), id
        """
    )
    db.commit()

    resident_vertices = int(db.execute("SELECT count(*) FROM gpu_resident_vertices").fetchone()[0])
    vertex_count = int(db.execute("SELECT count(*) FROM gpu_dense_ids").fetchone()[0])
    if vertex_count > 2_147_483_647:
        raise ValueError(
            f"GPU:{device_id} projection has {vertex_count:,} vertices; int32 dense IDs cannot represent it"
        )

    id_rows = iter(db.execute("SELECT id FROM gpu_dense_ids ORDER BY dense"))
    written_vertices = _write_u64_column(
        global_ids_path, id_rows, label=f"GPU:{device_id} dense GlobalId map"
    )
    if written_vertices != vertex_count:
        raise RuntimeError("GPU projection GlobalId artifact count changed during generation")

    # UNION (not UNION ALL) preserves the existing runtime semantics: parallel
    # logical relationships between the same two vertices become one BFS adjacency
    # pair.  Both orientations are emitted only when the corresponding endpoint is
    # GPU-resident, which is exactly what one-hop routing needs.
    edge_cursor = db.execute(
        """
        SELECT source_dense, destination_dense
        FROM (
            SELECT gs.dense - 1 AS source_dense, gd.dense - 1 AS destination_dense
            FROM edges e
            JOIN gpu_resident_vertices s ON s.id=e.src
            JOIN gpu_dense_ids gs ON gs.id=e.src
            JOIN gpu_dense_ids gd ON gd.id=e.dst

            UNION

            SELECT gs.dense - 1 AS source_dense, gd.dense - 1 AS destination_dense
            FROM edges e
            JOIN gpu_resident_vertices s ON s.id=e.dst
            JOIN gpu_dense_ids gs ON gs.id=e.dst
            JOIN gpu_dense_ids gd ON gd.id=e.src
            WHERE e.src != e.dst
        )
        ORDER BY source_dense, destination_dense
        """
    )

    adjacency_pairs = 0
    started = time.monotonic()
    last_report = started
    with sources_path.open("wb") as source_handle, destinations_path.open("wb") as destination_handle:
        while True:
            rows = edge_cursor.fetchmany(200_000)
            if not rows:
                break
            sources = array.array("I", (int(row[0]) for row in rows))
            destinations = array.array("I", (int(row[1]) for row in rows))
            if sys.byteorder != "little":
                sources.byteswap()
                destinations.byteswap()
            sources.tofile(source_handle)
            destinations.tofile(destination_handle)
            adjacency_pairs += len(rows)
            now = time.monotonic()
            if now - last_report >= 5.0:
                print(
                    f"[cache-plan] GPU:{device_id}: wrote {adjacency_pairs:,} dense adjacency pairs",
                    file=sys.stderr,
                    flush=True,
                )
                last_report = now

    total_bytes = (
        global_ids_path.stat().st_size
        + sources_path.stat().st_size
        + destinations_path.stat().st_size
    )
    # Conservative runtime estimate.  The reusable artifact removes graph parsing,
    # global->dense conversion, mirroring and CPU sort/dedup, but cuGraph still owns
    # COO input and construction workspace while producing its final CSR.
    estimated_peak_bytes = vertex_count * 16 + adjacency_pairs * 40
    manifest = {
        "format": "polygraph.gpu-projection.dense-edgelist",
        "version": 1,
        "deviceId": device_id,
        "residentVertices": resident_vertices,
        "vertices": vertex_count,
        "adjacencyPairs": adjacency_pairs,
        "bytes": total_bytes,
        "estimatedPeakDeviceBytes": estimated_peak_bytes,
        "globalIdRecord": "<uint64 canonical_global_id>",
        "denseEdgeRecord": "two parallel <uint32 dense_vertex_id> arrays",
        "globalIds": global_ids_path.name,
        "sources": sources_path.name,
        "destinations": destinations_path.name,
        "sortedUnique": True,
        "containsBothDirectionsForResidentSources": True,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    db.executescript(
        """
        DROP TABLE IF EXISTS temp.gpu_dense_ids;
        DROP TABLE IF EXISTS temp.gpu_projection_vertices;
        DROP TABLE IF EXISTS temp.gpu_resident_vertices;
        """
    )
    print(
        f"[cache-plan] GPU:{device_id}: reusable projection ready: "
        f"{resident_vertices:,} resident / {vertex_count:,} projection vertices, "
        f"{adjacency_pairs:,} adjacency pairs",
        file=sys.stderr,
        flush=True,
    )
    return GpuProjectionArtifact(
        device_id=device_id,
        manifest_path=manifest_path,
        global_ids_path=global_ids_path,
        sources_path=sources_path,
        destinations_path=destinations_path,
        resident_vertices=resident_vertices,
        vertices=vertex_count,
        adjacency_pairs=adjacency_pairs,
        bytes=total_bytes,
    )


def _existing_gpu_artifact_summary(output: Path, device_id: int) -> dict[str, Any] | None:
    root = output / "gpu" / f"gpu{device_id}"
    manifest_path = root / "manifest.json"
    if not manifest_path.is_file():
        return None
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if manifest.get("format") != "polygraph.gpu-projection.dense-edgelist":
        return None
    paths = [
        root / str(manifest.get("globalIds", "global_ids.bin")),
        root / str(manifest.get("sources", "sources.bin")),
        root / str(manifest.get("destinations", "destinations.bin")),
    ]
    if not all(path.is_file() for path in paths):
        return None
    vertex_count = int(manifest.get("vertices", -1))
    adjacency_pairs = int(manifest.get("adjacencyPairs", -1))
    if vertex_count < 0 or adjacency_pairs < 0:
        return None
    if paths[0].stat().st_size != vertex_count * UINT64.size:
        return None
    if paths[1].stat().st_size != adjacency_pairs * UINT32.size:
        return None
    if paths[2].stat().st_size != adjacency_pairs * UINT32.size:
        return None
    actual_bytes = sum(path.stat().st_size for path in paths)
    return {
        "manifest": str(Path("gpu") / f"gpu{device_id}" / "manifest.json"),
        "residentVertices": int(manifest.get("residentVertices", 0)),
        "vertices": vertex_count,
        "adjacencyPairs": adjacency_pairs,
        "bytes": actual_bytes,
        "estimatedPeakDeviceBytes": int(manifest.get("estimatedPeakDeviceBytes", 0)),
    }


def _write_residency_artifact(db: sqlite3.Connection, output: Path) -> dict[str, Any]:
    """Write selected cache residency independently from the immutable graph."""
    path = output / "residency.bin"
    entries = 0
    with path.open("wb") as handle:
        for signed_id, mask in db.execute(
            "SELECT id, cache_mask FROM nodes WHERE cache_mask != 0 ORDER BY (id < 0), id"
        ):
            handle.write(struct.pack("<QQ", canonical_global_id(_unsigned_sqlite_id(int(signed_id))), int(mask)))
            entries += 1
    return {
        "path": path.name,
        "entries": entries,
        "bytes": path.stat().st_size,
        "record": "<uint64 canonical_id,uint64 cache_location_mask>",
    }



def _existing_artifact_summary(output: Path, name: str) -> dict[str, Any] | None:
    manifest_path = output / name / "manifest.json"
    if not manifest_path.is_file():
        return None
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    index_path = manifest_path.parent / str(manifest.get("index", "index.bin"))
    neighbors_path = manifest_path.parent / str(manifest.get("neighbors", "neighbors.bin"))
    if not index_path.is_file() or not neighbors_path.is_file():
        return None
    expected_bytes = int(manifest.get("bytes", -1))
    actual_bytes = index_path.stat().st_size + neighbors_path.stat().st_size
    if expected_bytes >= 0 and expected_bytes != actual_bytes:
        return None
    return {
        "manifest": str(Path(name) / "manifest.json"),
        "vertices": int(manifest.get("vertices", 0)),
        "relations": int(manifest.get("relations", 0)),
        "bytes": actual_bytes,
    }


def _runtime_backends(
    config: dict[str, Any],
    output: Path,
    *,
    node_count: int,
    artifacts: dict[str, Any],
) -> list[dict[str, Any]]:
    priorities = dict(config.get("priorities", {}))
    result: list[dict[str, Any]] = []
    for gpu in config.get("gpus", []):
        if not gpu.get("enabled", True):
            continue
        device_id = int(gpu["device_id"])
        gpu_artifact = artifacts.get("gpu", {}).get(str(device_id), {})
        resident_vertices = int(gpu_artifact.get("residentVertices", 0) or 0)
        # Native cuGraph BFS is safe only when every canonical graph vertex is
        # resident on this GPU.  The dense projection may contain additional proxy
        # vertices for partial caches, so projection vertex_count alone is not a
        # completeness proof.  residentVertices is derived from cache_mask and is
        # therefore the authoritative residency count.
        complete_gpu_projection = node_count > 0 and resident_vertices == node_count
        result.append({
            "id": f"GPU:{device_id}",
            "kind": "CuGraph",
            "enabled": True,
            "name": str(gpu.get("name", f"GPU {device_id}")),
            "priority": int(gpu.get("priority", priorities.get(f"GPU:{device_id}", priorities.get("GPU", 0)))),
            "device_id": device_id,
            "memory_limit_bytes": _bytes(gpu.get("memory_limit_bytes", gpu.get("memory", 0))),
            "relation_limit": int(gpu.get("relation_limit", gpu.get("relations", 0)) or 0),
            "regions": gpu.get("regions", []),
            "projection_artifact": str(Path("gpu") / f"gpu{device_id}" / "manifest.json"),
            "connection": {"endpoint": "", "database": ""},
            "capabilities": {
                "batch_neighbors": True, "native_bfs": complete_gpu_projection,
                "property_filters": False, "gpu_execution": True,
                "replica_parallelism": False,
            },
            "coverage": {
                "mode": "id-routing",
                "fresh": True,
                "complete_for_requested_subgraph": complete_gpu_projection,
            },
        })

    redis_cfg = config.get("redis", {})
    if redis_cfg.get("enabled", False):
        result.append({
            "id": "Redis", "kind": "RedisCache", "enabled": True,
            "name": "Redis adjacency cache",
            "priority": int(redis_cfg.get("priority", priorities.get("Redis", 20))),
            "connection": {
                "endpoint": str(redis_cfg.get("endpoint", "127.0.0.1:6379")),
                "database": str(redis_cfg.get("database", 0)),
            },
            "key_prefix": str(redis_cfg.get("key_prefix", "polygraph:adj:")),
            "memory_limit_bytes": _bytes(redis_cfg.get("memory_limit_bytes", redis_cfg.get("memory", 0))),
            "vertex_limit": int(redis_cfg.get("vertex_limit", 0) or 0),
            "capabilities": {"batch_neighbors": True, "native_bfs": False, "property_filters": False},
            "coverage": {"mode": "id-routing", "fresh": True, "complete_for_requested_subgraph": False},
        })

    mmap_cfg = config.get("mmap", {})
    if mmap_cfg.get("enabled", False):
        mmap_artifact = artifacts.get("mmap", {})
        mmap_resident_vertices = int(mmap_artifact.get("vertices", 0) or 0)
        complete_mmap_cache = node_count > 0 and mmap_resident_vertices == node_count
        result.append({
            "id": "MMap", "kind": "MemoryMapped", "enabled": True,
            "name": "Memory-mapped adjacency cache",
            "priority": int(mmap_cfg.get("priority", priorities.get("MMap", 10))),
            "path": str((output / "mmap").resolve()),
            "memory_limit_bytes": _bytes(mmap_cfg.get("memory_limit_bytes", mmap_cfg.get("memory", 0))),
            "vertex_limit": int(mmap_cfg.get("vertex_limit", 0) or 0),
            "connection": {"endpoint": "", "database": ""},
            "capabilities": {"batch_neighbors": True, "native_bfs": False, "property_filters": False},
            "coverage": {
                "mode": "id-routing",
                "fresh": True,
                "complete_for_requested_subgraph": complete_mmap_cache,
            },
        })
    return result


def plan_graph_caches(
    graph_path: str | Path,
    cache_config: str | Path | dict[str, Any],
    *,
    output_dir: str | Path,
    routing_layout: BackendLayout | None = None,
    resume: bool = False,
    planning_db_path: str | Path | None = None,
) -> dict[str, Any]:
    graph = Path(graph_path).expanduser().resolve()
    output = Path(output_dir).expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    config = cache_config if isinstance(cache_config, dict) else load_cache_config(cache_config)
    metadata = open_graph_reader(graph).metadata()
    # Constructing the routing layout validates the graph's ID-routing metadata even
    # though cache residency is now stored independently from those owner bits.
    _ = routing_layout or BackendLayout(tuple(metadata.get("idRouting", ())))

    planning_db = (
        Path(planning_db_path).expanduser().resolve()
        if planning_db_path is not None
        else graph.parent / "cache-plan.sqlite3"
    )
    reuse_index = planning_db.is_file()
    if reuse_index:
        print(
            f"[cache-plan] reusing graph/index database {planning_db}",
            file=sys.stderr,
            flush=True,
        )
        try:
            db = _open_existing_planning_db(planning_db, graph)
        except (ValueError, sqlite3.DatabaseError):
            if resume:
                raise
            planning_db.unlink(missing_ok=True)
            db = _open_planning_db(planning_db)
            reuse_index = False
    else:
        db = _open_planning_db(planning_db)

    try:
        if reuse_index:
            node_count, relationship_count = _planning_db_counts(db)
            # A reusable planning DB stores graph/index facts, never one plan's residency.
            db.execute("UPDATE nodes SET cache_mask=0")
            db.commit()
            region_results: list[dict[str, Any]] = []
        else:
            print(
                f"[cache-plan] building bounded-memory graph/index database for {graph}",
                file=sys.stderr,
                flush=True,
            )
            node_count, relationship_count = _index_graph(graph, db)
            region_results: list[dict[str, Any]] = []

        cache_policies: list[tuple[str, dict[str, Any]]] = []
        for gpu in config.get("gpus", []):
            for region in gpu.get("regions", []):
                cache_policies.append(
                    (f"GPU:{gpu.get('device_id', '?')}/{region.get('name', 'region')}",
                     dict(region.get("policy", {})))
                )
        mmap_policy = config.get("mmap", {}).get("policy")
        if isinstance(mmap_policy, dict):
            cache_policies.append(("MMap", mmap_policy))
        redis_policy = config.get("redis", {}).get("policy")
        if isinstance(redis_policy, dict):
            cache_policies.append(("Redis", redis_policy))
        for policy_name, policy in cache_policies:
            owner_sources = {
                str(value)
                for value in policy.get("owner_sources", policy.get("ownerSources", []))
            }
            if "GPU" in owner_sources:
                raise ValueError(
                    f"Cache policy {policy_name!r} selects authoritative GPU-owned vertices. "
                    "Move original GPU placement to a GPU distribution config."
                )

        for gpu in config.get("gpus", []):
            if not gpu.get("enabled", True):
                continue
            device_id = int(gpu["device_id"])
            bit = gpu_location_bit(device_id)
            memory_limit = _bytes(gpu.get("memory_limit_bytes", gpu.get("memory", 0)))
            relation_limit = int(gpu.get("relation_limit", gpu.get("relations", 0)) or 0)
            memory_utilization = float(gpu.get("memory_utilization", 0.80))
            if not 0.0 < memory_utilization <= 1.0:
                raise ValueError("gpu.memory_utilization must be in the interval (0, 1]")
            # The old planner charged only 8 bytes per incident relationship, which
            # estimates final adjacency payload rather than peak cuGraph construction
            # memory.  A reusable dense artifact removes several temporaries, but the
            # device still owns COO input, CSR output and workspace concurrently.
            planning_bytes_per_relation = int(gpu.get("planning_bytes_per_relation", 40) or 40)
            vertex_overhead_bytes = int(gpu.get("vertex_overhead_bytes", 32) or 32)
            if planning_bytes_per_relation <= 0 or vertex_overhead_bytes < 0:
                raise ValueError("GPU planning byte estimates must be positive")
            usable_memory = int(memory_limit * memory_utilization) if memory_limit else 0
            remaining_memory = usable_memory
            remaining_relations = relation_limit
            for region_index, region in enumerate(gpu.get("regions", [])):
                policy = dict(region.get("policy", {}))
                if not policy:
                    policy = {"kind": region.get("kind", "top-degree")}
                if region.get("node_types") or region.get("nodeTypes"):
                    policy.setdefault("node_types", region.get("node_types", region.get("nodeTypes")))
                stats = _select_policy(
                    db,
                    policy=policy,
                    location_bit=bit,
                    memory_budget=max(0, remaining_memory),
                    relation_budget=max(0, remaining_relations),
                    vertex_budget=int(region.get("vertex_limit", region.get("vertices", 0)) or 0),
                    fixed_overhead_bytes=vertex_overhead_bytes,
                    neighbor_bytes=planning_bytes_per_relation,
                    excluded_sources=("GPU",),
                )
                if remaining_memory:
                    remaining_memory = max(0, remaining_memory - stats["estimatedBytes"])
                if remaining_relations:
                    remaining_relations = max(0, remaining_relations - stats["estimatedRelations"])
                region_results.append({
                    "location": f"GPU:{device_id}",
                    "deviceId": device_id,
                    "region": str(region.get("name", f"region-{region_index}")),
                    "priority": int(region.get("priority", gpu.get("priority", 0))),
                    "planningBytesPerRelation": planning_bytes_per_relation,
                    "memoryUtilization": memory_utilization,
                    **stats,
                })

        redis_cfg = config.get("redis", {})
        if redis_cfg.get("enabled", False):
            policy = dict(redis_cfg.get("policy", {"kind": "best"}))
            redis_memory_limit = _bytes(redis_cfg.get("memory_limit_bytes", redis_cfg.get("memory", 0)))
            redis_utilization = float(redis_cfg.get("memory_utilization", 0.90))
            if not 0.0 < redis_utilization <= 1.0:
                raise ValueError("redis.memory_utilization must be in the interval (0, 1]")
            redis_payload_budget = int(redis_memory_limit * redis_utilization) if redis_memory_limit else 0
            redis_vertex_budget = int(redis_cfg.get("vertex_limit", 0) or 0)
            redis_relation_budget = int(redis_cfg.get("relation_limit", 0) or 0)
            overhead = int(redis_cfg.get("entry_overhead_bytes", 128) or 128)

            # Default to a small deliberately shared hot tier, then spend the rest
            # on vertices not already resident on a GPU. This prevents Redis from
            # becoming a mostly-duplicate copy of the fastest cache.
            shared_fraction = float(redis_cfg.get("shared_hot_fraction", 0.075))
            if not 0.0 <= shared_fraction <= 1.0:
                raise ValueError("redis.shared_hot_fraction must be in [0, 1]")
            gpu_mask = 0
            for configured_gpu in config.get("gpus", []):
                if configured_gpu.get("enabled", True):
                    gpu_mask |= gpu_location_bit(int(configured_gpu["device_id"]))
            overlap_stats = {"vertices": 0, "estimatedRelations": 0, "estimatedBytes": 0}
            if gpu_mask and shared_fraction > 0.0:
                overlap_policy = dict(policy)
                if redis_vertex_budget:
                    overlap_policy["vertex_count"] = max(1, int(redis_vertex_budget * shared_fraction))
                overlap_stats = _select_policy(
                    db, policy=overlap_policy, location_bit=LOCATION_REDIS,
                    memory_budget=int(redis_payload_budget * shared_fraction) if redis_payload_budget else 0,
                    relation_budget=int(redis_relation_budget * shared_fraction) if redis_relation_budget else 0,
                    vertex_budget=max(1, int(redis_vertex_budget * shared_fraction)) if redis_vertex_budget else 0,
                    fixed_overhead_bytes=overhead, require_mask=gpu_mask,
                    excluded_sources=("GPU",),
                )

            remaining_memory = max(0, redis_payload_budget - overlap_stats["estimatedBytes"]) if redis_payload_budget else 0
            remaining_relations = max(0, redis_relation_budget - overlap_stats["estimatedRelations"]) if redis_relation_budget else 0
            remaining_vertices = max(0, redis_vertex_budget - overlap_stats["vertices"]) if redis_vertex_budget else 0
            if redis_vertex_budget and remaining_vertices == 0:
                unique_stats = {"vertices": 0, "estimatedRelations": 0, "estimatedBytes": 0}
            elif redis_payload_budget and remaining_memory == 0:
                unique_stats = {"vertices": 0, "estimatedRelations": 0, "estimatedBytes": 0}
            else:
                unique_stats = _select_policy(
                    db, policy=policy, location_bit=LOCATION_REDIS,
                    memory_budget=remaining_memory, relation_budget=remaining_relations,
                    vertex_budget=remaining_vertices, fixed_overhead_bytes=overhead,
                    exclude_mask=gpu_mask, excluded_sources=("GPU",),
                )
            stats = {
                "vertices": int(overlap_stats["vertices"]) + int(unique_stats["vertices"]),
                "estimatedRelations": int(overlap_stats["estimatedRelations"]) + int(unique_stats["estimatedRelations"]),
                "estimatedBytes": int(overlap_stats["estimatedBytes"]) + int(unique_stats["estimatedBytes"]),
            }
            region_results.append({
                "location": "Redis", "region": "redis",
                "priority": int(redis_cfg.get("priority", 20)),
                "sharedHotFraction": shared_fraction,
                "sharedHotVertices": overlap_stats["vertices"],
                "redisUniqueVertices": unique_stats["vertices"],
                "candidateVertices": unique_stats.get("candidateVertices", 0),
                "rankSkippedVertices": unique_stats.get("rankSkippedVertices", 0),
                "requestedVertexFraction": unique_stats.get("requestedVertexFraction"),
                **stats,
            })

        mmap_cfg = config.get("mmap", {})
        if mmap_cfg.get("enabled", False):
            policy = dict(mmap_cfg.get("policy", {"kind": "best"}))
            include_gpu_owned = bool(mmap_cfg.get("include_gpu_owned", mmap_cfg.get("includeGpuOwned", False)))
            excluded_cache_mask = _cache_location_mask(
                policy.get("exclude_locations", policy.get("excludeLocations", ()))
            )
            stats = _select_policy(
                db,
                policy=policy,
                location_bit=LOCATION_MMAP,
                memory_budget=_bytes(mmap_cfg.get("memory_limit_bytes", mmap_cfg.get("memory", 0))),
                relation_budget=int(mmap_cfg.get("relation_limit", 0) or 0),
                vertex_budget=int(mmap_cfg.get("vertex_limit", 0) or 0),
                excluded_sources=() if include_gpu_owned else ("GPU",),
                exclude_mask=excluded_cache_mask,
            )
            region_results.append({
                "location": "MMap",
                "region": "mmap",
                "priority": int(mmap_cfg.get("priority", 10)),
                "excludedCacheMask": excluded_cache_mask,
                **stats,
            })
        mmap_cfg = config.get("mmap", {})
        redis_cfg = config.get("redis", {})
        artifacts: dict[str, Any] = {}
        gpu_artifacts: dict[str, Any] = {}
        for gpu in config.get("gpus", []):
            if not gpu.get("enabled", True):
                continue
            device_id = int(gpu["device_id"])
            existing = _existing_gpu_artifact_summary(output, device_id) if resume else None
            if existing is not None:
                print(
                    f"[cache-plan] reusing completed GPU:{device_id} projection artifact",
                    file=sys.stderr,
                    flush=True,
                )
                gpu_artifacts[str(device_id)] = existing
            else:
                artifact = _build_gpu_projection_artifact(
                    db,
                    output / "gpu" / f"gpu{device_id}",
                    device_id,
                    gpu_location_bit(device_id),
                )
                manifest = json.loads(artifact.manifest_path.read_text(encoding="utf-8"))
                memory_limit = _bytes(gpu.get("memory_limit_bytes", gpu.get("memory", 0)))
                estimated_peak = int(manifest.get("estimatedPeakDeviceBytes", 0))
                if memory_limit and estimated_peak > memory_limit:
                    raise ValueError(
                        f"GPU:{device_id} cache projection is estimated to require "
                        f"{estimated_peak:,} peak device bytes, above the configured "
                        f"{memory_limit:,}; reduce GPU cache size/relation_limit"
                    )
                gpu_artifacts[str(device_id)] = {
                    "manifest": str(Path("gpu") / f"gpu{device_id}" / "manifest.json"),
                    "residentVertices": artifact.resident_vertices,
                    "vertices": artifact.vertices,
                    "adjacencyPairs": artifact.adjacency_pairs,
                    "bytes": artifact.bytes,
                    "estimatedPeakDeviceBytes": estimated_peak,
                }
        if gpu_artifacts:
            artifacts["gpu"] = gpu_artifacts

        if mmap_cfg.get("enabled", False):
            existing = _existing_artifact_summary(output, "mmap") if resume else None
            if existing is not None:
                print("[cache-plan] reusing completed mmap artifact", file=sys.stderr, flush=True)
                artifacts["mmap"] = existing
            else:
                artifact = _build_binary_artifact(db, output / "mmap", "mmap", LOCATION_MMAP)
                artifacts["mmap"] = {
                    "manifest": str(Path("mmap") / "manifest.json"),
                    "vertices": artifact.vertices,
                    "relations": artifact.relations,
                    "bytes": artifact.bytes,
                }
        if redis_cfg.get("enabled", False):
            existing = _existing_artifact_summary(output, "redis") if resume else None
            if existing is not None:
                print("[cache-plan] reusing completed Redis artifact", file=sys.stderr, flush=True)
                artifacts["redis"] = existing
            else:
                artifact = _build_binary_artifact(db, output / "redis", "redis", LOCATION_REDIS)
                artifacts["redis"] = {
                    "manifest": str(Path("redis") / "manifest.json"),
                    "vertices": artifact.vertices,
                    "relations": artifact.relations,
                    "bytes": artifact.bytes,
                }

        plan = {
            "format": "polygraph.cache-plan",
            "version": 1,
            "graph": str(graph),
            "nodes": node_count,
            "relationships": relationship_count,
            "graphRouting": {
                "idRouting": list(metadata.get("idRouting", [])),
                "idRoutingBits": metadata.get("idRoutingBits"),
            },
            "locationEncoding": "external-residency-v1",
            "regions": region_results,
            "artifacts": artifacts,
            "priorities": config.get("priorities", {}),
            "gpus": config.get("gpus", []),
            "mmap": mmap_cfg,
            "redis": redis_cfg,
            "planningDatabase": str(planning_db),
            "resumed": bool(resume),
            "reusedPlanningDatabase": bool(reuse_index),
            "runtimeBackends": _runtime_backends(config, output, node_count=node_count, artifacts=artifacts),
        }

        # Cache residency is an independent artifact. The canonical graph remains
        # immutable and can therefore be reused across arbitrarily many plans.
        plan["artifacts"]["residency"] = _write_residency_artifact(db, output)
        partial_plan_path = output / "cache-plan.partial.json"
        partial_plan_path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")

        plan_path = output / "cache-plan.json"
        plan_path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
        partial_plan_path.unlink(missing_ok=True)
        return {**plan, "plan": str(plan_path)}
    finally:
        db.close()
        # The graph/index database is deliberately persistent: creating another
        # cache configuration should not require reparsing/reindexing graph.jsonl.
        if planning_db.exists():
            print(
                f"[cache-plan] reusable planning database: {planning_db}",
                file=sys.stderr,
                flush=True,
            )
