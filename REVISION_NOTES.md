# BFS, argparse, compact GPU mapping, and metrics revision

## 1. Why the BFS returned only the source

The LDBC loader rewrites backend-owned IDs after loading and writes a sorted
`cpgdb.native-id-map` JSONL sidecar. `tools/cpgdb.py run` already translated IDs
from `start_uuids.txt`, but the C++ runtime did not read `domain.paths.nativeIdMap`.
Calling the HTTP endpoint or BFS executable directly with an original start ID
therefore queried an ID that was not present in the freshly loaded backend. The
BFS state still inserted that value at distance zero, while expansion returned no
rows, producing the source-only result.

The runtime now installs a sparse, lazy `NativeNodeIdResolver` whenever the domain
contains `paths.nativeIdMap`. It stores one index entry per 4096 node mappings and
caches only IDs that are actually queried. The standalone compatibility mode also
accepts `--native-id-map <path>`.

The HTTP BFS response now reports:

```json
{
  "requestedSourceVertexId": "559308805355328126",
  "sourceVertexId": "<resolved-loaded-id>",
  "sourceVertexIdWasRemapped": true,
  "maxDepth": 7,
  "result": {}
}
```

This makes a stale/original input ID visible immediately.

## 2. Command-line parser

`src/cli/CommandLine.cpp` is now an adapter over `p-ranav/argparse` v3.2. The
existing `CommandLine` public interface was preserved, so the BFS application did
not need a wide call-site rewrite. CMake first tries `find_package(argparse)` and
falls back to `FetchContent` when dependency fetching is enabled.

## 3. Compact GPU host state

The GPU runtime no longer retains:

- full `Node` and `Edge` objects,
- node/relationship properties and labels,
- `Record` wrappers,
- the host adjacency hash-of-hashes,
- a second `unordered_map<GlobalId, dense_id>`.

After projection construction, it retains only a sorted
`std::vector<GlobalId>` that maps dense GPU vertex IDs back to CPGDB IDs. Mapping
an input UUID to a dense ID uses binary search. Dense-to-global result conversion
remains constant time.

The edge endpoint vector and dense source/destination arrays are temporary build
inputs and are released after cuGraph construction.

### Compact-projection contract

The current projection is mirrored for undirected expansion and does not retain
attributes. Therefore:

- undirected or `Both`, unfiltered BFS is supported;
- directed GPU expansion requires a separate directed projection;
- node/edge type filters cannot be evaluated by this compact GPU projection.

Unsupported GPU specifications now fail with an explicit error instead of
silently discarding every sampled candidate.

## 4. Prometheus and Grafana

The active projection exports fixed, label-free gauges:

```text
cpgdb_gpu_projection_vertices
cpgdb_gpu_projection_input_edges
cpgdb_gpu_projection_raw_adjacency_pairs
cpgdb_gpu_projection_unique_adjacency_pairs
cpgdb_gpu_projection_duplicate_adjacency_pairs_removed
cpgdb_gpu_projection_host_id_mapping_required_bytes
cpgdb_gpu_projection_host_id_mapping_allocated_bytes
cpgdb_gpu_projection_device_vertex_input_bytes
cpgdb_gpu_projection_device_edge_input_bytes
cpgdb_gpu_projection_build_duration_seconds
cpgdb_gpu_projection_mirrored_for_undirected_expansion
```

`host_id_mapping_required_bytes` is exactly the UUID payload size.
`host_id_mapping_allocated_bytes` reflects vector capacity and is the better
number for actual retained host allocation. These values are updated once when
the projection is built; `/metrics` only serializes them.

The provisioned Grafana dashboard now contains a GPU projection row with UUID-map
RAM, vertex/adjacency counts, and build-input memory.

## Validation performed in this archive

- Non-GPU syntax checks passed for all changed CLI/runtime/factory/metrics-access
  translation units.
- `MetricsService.cpp` passed a syntax check against a prometheus-cpp API stub.
- `GpuProjectionBuilder` and `CuGraphProjectionStore` passed instantiated/header
  syntax checks against CUDA/cuGraph API stubs.
- Executable checks passed for the compact `DenseIdMapper` and sparse
  `NativeNodeIdResolver`.
- The Grafana dashboard JSON parses successfully.

A real CUDA/cuGraph build and traversal cannot be executed in the packaging
sandbox because the RAPIDS/CUDA toolchain and running graph backends are not
available. Build it in the configured RAPIDS environment with:

```bash
cmake --preset gpu-bfs
cmake --build --preset gpu-bfs
```


python3 tools/measurement/arango_bfs_sweep.py   --cache-configs config/cache/benchmark/sf1/*.json   --http-request-plan config/benchmarks/sf1-cache-http-requests.example.json   --cache-root data/ldbc-snb/sf1_0/schema-based/caches/   --planning-db data/ldbc-snb/sf1_0/schema-based/cache-plan.sqlite3   --base-output-dir measurements/sf1-0_benchmarks/arango-sweep   --concurrency 4,8,16,32   --batch-sizes 10000,25000,50000,100000,250000   --keep-going   ldbc-snb-sf1_0