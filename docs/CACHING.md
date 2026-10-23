# Cache planning and runtime residency

## Design goal

Cache strategy must be changeable without mutating, redistributing, or re-identifying the graph. PolyGraph therefore separates **canonical graph identity** from **cache residency**.

```text
graph.jsonl
    canonical immutable graph

cache-plan.sqlite3
    reusable graph/index database

cache configuration A
    ↓
cache-A/
    residency.bin
    gpu/
      gpu0/
        global_ids.bin
        sources.bin
        destinations.bin
        manifest.json
    redis/
    mmap/
    cache-plan.json

cache configuration B
    ↓
cache-B/
    residency.bin
    redis/
    mmap/
    cache-plan.json
```

At runtime the effective routing state is conceptually:

```text
canonical logical owner
+ authoritative GPU physical ownership (when owner == GPU)
+ selected cache plan residency
= routable BFS vertex
```

Authoritative GPU placement is not cache residency. A separate `gpu-storage-plan.json` maps logical GPU-owned canonical IDs to physical `GPU:N` devices and provides reusable base GPU projection artifacts. When an external `--cache-plan` is selected, its `residency.bin` contributes only additional cache locations. Legacy cache bits embedded in old graph IDs are ignored; the immutable logical owner still survives from the graph ID.

## `cache-plan.sqlite3`

The planner streams `graph.jsonl` into a disk-backed SQLite database containing canonical node IDs, source/type information, degree, and an edge table with source/destination indexes. It is intentionally retained after a successful plan.

The DB stores a graph signature (resolved path, size, and modification timestamp). Reuse is allowed only when the signature matches the current graph. Old/incomplete databases or a database from another graph revision are rebuilt rather than reused silently.

The cache mask in SQLite is temporary plan state. Before producing a new cache configuration it is reset to zero; graph/index facts and degree/index work are reused. GPU/Redis/MMAP artifacts are then generated from SQLite, so `graph.jsonl` is not rescanned merely to build another cache configuration.

## `residency.bin`

Each plan writes only selected cache locations:

```text
<uint64 canonical_id, uint64 cache_location_mask>
```

The file contains one record per vertex with non-zero cache residency. It is already sorted by canonical ID. Runtime reads it in one bulk binary operation into a flat sorted index; it no longer inserts millions of entries into a `std::unordered_map`.

This avoids rewriting millions of JSONL records simply to try a different GPU/Redis/MMAP split and keeps cache-plan startup bounded and allocation-light.

## Authoritative GPU storage versus GPU cache

Vertices whose authoritative graph source is `GPU` are materialized independently of caching. Preparation writes:

```text
gpu-storage/
  gpu-storage-plan.json
  ownership.bin
  gpu/
    gpu0/
      global_ids.bin
      sources.bin
      destinations.bin
      manifest.json
    gpu1/
      ...
```

`ownership.bin` maps each canonical GPU-owned vertex to exactly one physical `GPU:N`. Canonical IDs do not change when the physical distribution changes. A dedicated GPU distribution configuration controls this partitioning; when none is supplied, authoritative GPU data defaults to GPU 0 for compatibility.

Cache configuration contains acceleration replicas only. GPU, Redis, and MMAP cache selection automatically excludes vertices whose authoritative source is `GPU`. A GPU-owned canonical ID can still appear as a neighbor endpoint inside a cache adjacency artifact; that preserves correct expansion output without making the endpoint cache-resident.

## GPU cache selection and reusable projection artifacts

A GPU cache configuration defines one or more cache regions. Typical policies are:

- `top-degree` / `best` — select high-degree non-GPU-owned vertices under vertex, relation, and memory limits.
- `all` — select every eligible non-GPU-owned vertex that fits the configured limits.
- `include_neighbors: true` — promote eligible non-GPU-owned neighbors while honoring the configured budgets.

A cache region using `owner_sources: ["GPU"]` is rejected. Original GPU distribution belongs in the separate GPU distribution configuration.

GPU planning uses a conservative construction-memory estimate by default (`memory_utilization = 0.80`, `planning_bytes_per_relation = 40`) rather than treating raw adjacency bytes as peak cuGraph memory. Both values can be overridden per GPU when hardware measurements justify it.

After residency is selected, `cache build` materializes a reusable dense projection under `gpu/gpuN/`:

```text
global_ids.bin       dense ID -> canonical GlobalId
sources.bin          uint32 dense source IDs
destinations.bin     uint32 dense destination IDs
manifest.json        counts, format and estimated peak bytes
```

This work is performed from the retained `cache-plan.sqlite3` graph/index database. The artifact already contains both logical directions needed for `both`/undirected one-hop expansion from GPU-resident sources, is sorted and deduplicated, and uses the final dense ID order.

A normal runtime startup therefore **does not scan `graph.jsonl`** for a newly prepared domain. The authoritative GPU projection is loaded from `gpu-storage/gpu/gpuN/`; an optional GPU cache projection is loaded independently from the selected cache plan. When both target the same physical GPU, their sorted dense-ID/edge artifacts are merged before cuGraph construction. Old domains without `gpu-storage-plan.json` retain the legacy graph-scan fallback for compatibility.

The cuGraph device object itself remains process-local. Restarting the server still requires a host-to-device upload and cuGraph CSR construction, but it no longer requires JSON parsing, graph-wide filtering, GlobalId collection, CPU renumbering, edge mirroring, or CPU sort/dedup.

## Redis and GPU overlap

Redis should not simply duplicate the GPU hot set. By default PolyGraph uses:

```json
"shared_hot_fraction": 0.075
```

The Redis planner first spends about 7.5% of its configured budget on intentionally shared hot GPU vertices, then fills the remainder from vertices **not resident on any configured GPU**. The exclusion also applies to neighbor promotion.

This keeps a small shared ultra-hot tier while giving Redis substantial unique coverage so it is actually exercised during BFS.

Set `redis.shared_hot_fraction` between `0.0` and `1.0` to tune this behavior.

## Redis materialization and readiness

Generated Redis adjacency lives on disk under `<cache>/redis/` until explicitly materialized:

```bash
python3 tools/polygraph.py cache redis-load DOMAIN \
  --cache-plan CACHE/cache-plan.json
```

The loader:

1. connects to the Redis backend declared by the selected cache plan;
2. resets the Redis database by default;
3. deletes the readiness marker before loading;
4. streams adjacency keys in batches, reporting progress every ~5 seconds;
5. writes `__manifest` only after the complete plan is loaded.

The C++ Redis store checks that marker before it advertises `can_expand()`. It also verifies that the marker names the **same cache-plan path** selected by the runtime. An empty Redis instance or Redis materialized from another plan is therefore skipped and BFS falls back to the authoritative backend instead of failing from a stale residency bit.

If a selected source key is nevertheless missing after readiness was established, the store invalidates readiness and raises an explicit incomplete-materialization error.

## Runtime backend configuration

Persistent backend configuration remains authoritative-data-only. It may reference `gpuStoragePlan`, whose `runtimeBackends` describe physical GPUs used by authoritative GPU storage. Cache plans independently carry runtime backend entries for GPU/Redis/MMAP acceleration. `GraphRuntimeBuilder` applies authoritative GPU runtime backends first, then merges the selected cache plan's runtime entries.

This keeps cache A and cache B independently selectable without changing `domain.json`, `graph.jsonl`, Neo4j, ArangoDB, or the authoritative GPU partition.

## Build several plans

```bash
python3 tools/polygraph.py cache build DOMAIN \
  --config config/cache/a.json config/cache/b.json config/cache/c.json
```

Useful options:

- `--output-dir DIR` — cache root; default is `<domain-directory>/caches`.
- `--planning-db FILE` — reusable SQLite path; default is beside `graph.jsonl`.
- `--load-redis` — immediately materialize Redis for each Redis-enabled plan.
- `--redis-no-reset` — opt out of the safe Redis reset.
- `--redis-batch-size N` — default `10000`.

## Runtime dependency on `graph.jsonl`

New GPU storage and cache plans store the graph's ID-routing metadata. `GraphRuntimeBuilder` validates routing from those plans and loads reusable binary GPU projections rather than reopening the canonical graph. Therefore newly prepared BFS/server startup uses the schema, backend configuration, authoritative GPU storage plan, optional cache plan, and their binary artifacts; `graph.jsonl` is an offline/preparation input rather than a hot startup dependency.

The JSONL graph scan is retained only as a compatibility fallback for older prepared domains that do not have a `gpu-storage-plan.json`.
