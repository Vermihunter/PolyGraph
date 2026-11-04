# PolyGraph · Developer wiki

**A guided tour through PolyGraph's architecture, implementation, operations, and experiments.**

PolyGraph is a C++23 research runtime that executes graph traversals over a **single logical graph** whose data can live in Neo4j, ArangoDB, PostgreSQL, GPU/cuGraph, Redis, or memory-mapped caches. It separates logical identity, authoritative placement, cache residency, and execution decisions.

> **Where to start:** New contributors should read [Getting started](guides/GETTING_STARTED.md), then [Architecture](architecture/ARCHITECTURE.md). For exact types and functions, use the sidebar **Classes**, **Namespaces**, and **Files** views.

## Learning paths

| I want to… | Read |
|:--|:--|
| Build the project and run my first BFS | [Getting started](guides/GETTING_STARTED.md) → [Build presets](guides/BUILDING.md) |
| Understand the request and data paths | [Architecture](architecture/ARCHITECTURE.md) → [Backend scheduling](architecture/BACKEND_REQUEST_SCHEDULING.md) |
| Understand ownership and caching | [Storage](architecture/STORAGE.md) → [Caching and residency](architecture/CACHING.md) |
| Prepare LDBC data and repeat benchmarks | [Data loading](guides/DATA_LOADING.md) → [Benchmarking](guides/BENCHMARKING.md) |
| Extend the system safely | [API conventions](reference/API_CONVENTIONS.md) → [API map](reference/API_REFERENCE.md) → [Testing & independent BFS oracle](guides/TESTING.md) |
| Operate and debug an instance | [Observability](operations/OBSERVABILITY.md) → [Troubleshooting](guides/TROUBLESHOOTING.md) |

## Explore by area

### Guides · do something

[Getting started](guides/GETTING_STARTED.md) · [Building](guides/BUILDING.md) · [Data loading](guides/DATA_LOADING.md) · [Native bulk ingestion](guides/BULK_INGESTION.md) · [LDBC fast preparation](guides/FAST_LDBC_PREPARATION.md) · [Benchmarking](guides/BENCHMARKING.md) · [Scripts](guides/SCRIPTS.md) · [Testing](guides/TESTING.md) · [Troubleshooting](guides/TROUBLESHOOTING.md)

### Architecture · understand how it works

[Runtime architecture](architecture/ARCHITECTURE.md) · [Storage model](architecture/STORAGE.md) · [Insertion](architecture/INSERTION.md) · [Backend request scheduling](architecture/BACKEND_REQUEST_SCHEDULING.md) · [Caching](architecture/CACHING.md) · [Direction-optimized BFS](algorithms/DIRECTION_OPTIMIZED_BFS.md)

### Reference · look up contracts

[API reference map](reference/API_REFERENCE.md) · [API conventions](reference/API_CONVENTIONS.md) · [Configuration](reference/CONFIGURATION.md) · [PostgreSQL adapter](reference/POSTGRESQL.md)

### Operations · inspect and maintain

[Observability](operations/OBSERVABILITY.md) · [Static analysis](operations/STATIC_ANALYSIS.md)

### Research · reproduce, do not assume

[GPU experiments and findings](research/GPU_PERFORMANCE_EXPERIMENTS.md) · [Distributed GPU integration status](research/DISTRIBUTED_GPU.md) · [Standalone multi-GPU and MPI probes](../src/gpu/distributed/README.md) · [Arango sweep](research/ARANGO_CHUNK_SWEEP.md) · [SF1 distribution matrix](benchmarks/SF1_DISTRIBUTION_MATRIX.md) · [SF1 cache research](benchmarks/SF1_CACHE_RESEARCH.md) · [Benchmark notes](benchmarks/LOW_MEMORY_BFS_EXPERIMENT_V4.md) · [Historical revision notes](history/REVISION_NOTES.md)

## Source reference

The generated website automatically indexes C++ headers and implementation files under `include/`, `src/`, and `app/`. Use **Search** for symbols, or explore **Modules / Classes / Files / Namespaces** from the navigation sidebar. Markdown guides are indexed and searchable alongside code; there is no separate hand-maintained HTML copy.

## Build this wiki

```bash
cmake --preset docs
cmake --build --preset docs
# open build/docs/docs/generated/html/index.html
```

Doxygen is required; Graphviz is optional. The `docs` preset configures **without** fetching C++ libraries, CUDA, RAPIDS, or backend dependencies. Output is generated beneath `build/` and is never committed.
