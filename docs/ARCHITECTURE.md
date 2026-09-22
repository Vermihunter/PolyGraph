# Architecture

## Runtime layers

PolyGraph presents one logical graph over heterogeneous storage/execution backends. The main runtime path is:

```text
CLI / HTTP / gRPC
       ↓
DatabaseContext + LogicalGraphSchema
       ↓
PlacementCatalog
       ↓
BFS executor / query model
       ↓
IGraphStore adapters
  ├─ Neo4j
  ├─ ArangoDB
  ├─ GPU/cuGraph
  ├─ Redis cache
  └─ memory-mapped cache
```

`GraphRuntimeBuilder` loads the domain/schema/backend configuration, validates the ID-routing layout, applies an optional cache plan, creates backend stores, and builds GPU projections where configured.

## Authoritative ownership versus cache residency

A global ID contains the stable logical authoritative owner plus the local payload. Cache placement is no longer required to be encoded by rewriting the graph. Logical GPU ownership remains stable in the ID, while physical multi-GPU placement is supplied separately by `gpu-storage/ownership.bin`:

```text
logical owner == GPU
        +
ownership[canonical_id]
        ↓
physical owner == GPU:0 / GPU:1 / ...
```

With no external cache plan, legacy embedded cache-location bits are still understood for compatibility. With `--cache-plan`, the runtime intentionally ignores those legacy cache bits and computes locations from the physical authoritative owner plus selected-plan residency. This guarantees that selecting cache B cannot accidentally inherit Redis/GPU bits from cache A, and moving authoritative GPU data between devices does not rewrite canonical IDs.

See [CACHING.md](CACHING.md).

## PlacementCatalog

`PlacementCatalog` is the fast runtime router. For every frontier vertex it:

1. decodes the stable logical authoritative owner;
2. resolves logical GPU ownership through the physical GPU ownership table when present;
3. merges external cache residency when enabled;
4. identifies resident stores;
5. checks whether a store can answer the requested `ExpandSpec`;
6. chooses the best eligible backend by priority.

A cache is therefore an optional execution location, not the source of graph identity.

## BFS execution

The distributed BFS executor groups frontier vertices by selected backend and issues batch-neighbor operations. Neo4j/ArangoDB/PostgreSQL can serve persistent adjacency; Redis/MMAP provide precomputed unfiltered adjacency; GPU/cuGraph provides accelerated projection execution.

The runtime can also choose native pushdown when a backend advertises complete/fresh coverage and the BFS options allow it.

## GPU projection construction

For newly prepared domains, expensive graph-wide selection/renumbering is an offline step. Preparation materializes reusable dense projections separately for:

- authoritative GPU storage (`gpu-storage/gpu/gpuN/`);
- optional GPU cache residency (`caches/<plan>/gpu/gpuN/`).

Runtime bulk-loads those arrays. If a physical GPU has both authoritative and cache projections, `GraphRuntimeBuilder` unions their sorted canonical-ID sets, remaps the two dense edge streams, deduplicates them, and then builds one cuGraph device graph. The device object remains process-local, so host-to-device upload and cuGraph CSR construction still occur on startup.

Older domains without a GPU storage plan retain the legacy `graph.jsonl` projection scan only as a compatibility fallback.

## Redis safety

Redis is treated as usable only when its readiness manifest exists and names the same selected cache plan. This protects BFS from stale or empty Redis state even if a plan says a vertex is Redis-resident.

## Configuration composition

`domain.json` references the stable schema/data/backend/observability files. Persistent backend configuration describes authoritative data. A selected `cache-plan.json` supplies cache runtime backends and `residency.bin` at startup, allowing many cache plans to target the same domain.
