# C++ API reference

This is the maintained high-level source reference. For exact signatures, treat headers under `include/` as authoritative and generate Doxygen with the `docs` preset.

## Traversal

### `BfsExecutor`

`include/algorithms/bfs/BfsExecutor.hpp`

Runs BFS over a `PlacementCatalog`, allowing the runtime to select persistent or accelerated stores per frontier. Options live in `BfsOptions`; output is `BfsResult`.

### `ExpandSpec`

`include/algorithms/ExpandSpec.hpp`

Describes traversal direction and optional edge/target-node filters. Cache stores may support only a subset; `can_expand()` is checked before routing.

## Runtime and placement

### `DatabaseContext`

`include/infrastructure/DatabaseContext.hpp`

Owns/coordinates schema, backend registry/configuration, placement catalog, GPU runtimes, graph version, and optional input-ID resolution.

### `PlacementCatalog`

`include/infrastructure/PlacementCatalog.hpp`

Maps each frontier vertex to the best eligible resident backend. `set_id_routing_backends()` configures authoritative ID decoding; `set_cache_residency()` enables external selected-plan residency.

### `ConfiguredGraphStoreFactory`

`include/infrastructure/ConfiguredGraphStoreFactory.hpp`

Constructs concrete stores from backend JSON and runtime inputs, including GPU projection creation.

### `GraphRuntimeBuilder`

`include/cli/GraphRuntimeBuilder.hpp`

Builds `DatabaseContext` from schema/backend configuration, an authoritative GPU storage plan when present, and an optional cache plan. New domains bulk-load the physical GPU ownership table plus reusable base/cache GPU projection binaries without scanning `graph.jsonl`; old domains retain the legacy projection-scan fallback.

## Store interface

### `IGraphStore`

`include/data_access/IGraphStore.hpp`

Common graph-store contract. Important operations include capability checks, batch neighbor expansion, minimal adjacency expansion, and optional native BFS support.

Concrete adapters live under `include/data_access/adapters/` and corresponding `src/data_access/adapters/` sources.

## Data model

- `GlobalId`: `include/data_model/EntityId.hpp`
- `NodeRef`: lightweight routable node reference.
- `EdgeRef`: lightweight edge reference.
- `AdjacencyRow` / `MinimalAdjacencyRow`: batch expansion output.
- `Node`, `Edge`, `Path`, `Record`, `Value`: richer query/result model.

## ID encoding

### `BackendEncodedId`

`include/insertion/BackendEncodedUuid.hpp`

Despite the compatibility filename, the API operates on uint64 IDs. It provides authoritative location masks, GPU/Redis/MMAP location bits, owner decoding, payload extraction, canonicalization, and legacy cache-residency decoration helpers.

## GPU

- `GpuRuntime`: RAFT/cuGraph runtime holder.
- `DenseIdMapper`: global uint64 ↔ dense GPU ID mapping.
- `GpuProjectionBuilder`: builds CSR either from legacy GlobalId edge input or directly from prebuilt dense authoritative/cache GPU projection artifacts.
- `CsrGraphProjection`: projection/storage statistics.
- `CuGraphBfsExecutor`: GPU BFS execution.

## Query model

`Query`, `QueryVisitor`, and query types under `include/query/query_types/` provide backend-specific query generation/execution for vertex lookup, neighbors, insertion, and graph scans.

## Events and logging

### Events

`include/events/` contains compile-time event gathering interfaces, event scopes, serializers, sinks, and setup. Events are structured execution data and typically end in JSONL.

### Logging

`include/logging/` contains operational logging macros/setup. Logs are human-readable and go to stdout/text files.

## Metrics

`include/observability/` contains observability configuration, `MetricsAccess`, metric types, collectors, and `MetricsService`. Server metrics are exposed by `MetricsController` when enabled.

## Frontends

- HTTP controllers/services under `include/server/http`, `controllers`, and `services`.
- gRPC traversal service under `include/server/grpc`.
- BFS CLI helpers under `include/cli`.

## Generate Doxygen

```bash
cmake --preset docs
cmake --build --preset docs
```

The generated output reflects exact signatures and source comments for the checked-out revision.
