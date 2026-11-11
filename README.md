# PolyGraph

**One logical graph. Multiple storage engines. CPU and GPU execution.**

[Graph quickstart](docs/graphs/quickstart.md) · [Add a dataset](docs/graphs/adding-a-dataset.md) · [Documentation](docs/README.md) · [Getting started](docs/guides/GETTING_STARTED.md) · [Architecture](docs/architecture/ARCHITECTURE.md) · [Native bulk ingestion](docs/guides/BULK_INGESTION.md) · [Docker load](docs/guides/DOCKER_INGESTION.md) · [Graph partitioning](docs/guides/GRAPH_PARTITIONING.md) · [Benchmarks](docs/guides/BENCHMARKING.md) · [C++ API](docs/reference/API_REFERENCE.md)

---

PolyGraph is a **C++23 heterogeneous graph execution engine and research platform**. Rather than forcing graph data into a single database, it combines persistent backends, accelerated projections, and optional caches behind a shared traversal interface. The central engineering problem: **where should each part of a traversal execute when data and compute are distributed across different systems?**

It is designed for experimenting with correctness, data placement, scheduling, and measured performance—not for claiming that a synthetic benchmark alone proves production superiority.

## The system at a glance

```text
                         CLI / Drogon HTTP API
                                 │
                   GraphRuntimeBuilder / DatabaseContext
                                 │
       LogicalGraphSchema ── PlacementCatalog ── Backend scheduler
                                 │
                       BFS / NeighborExpander
                                 │
                 IGraphStore (batch expansion contract)
                       ├─ Neo4j          ├─ ArangoDB
                       ├─ PostgreSQL     ├─ GPU/cuGraph
                       └─ Redis          └─ memory-mapped cache
```

**What is distinctive?** Logical vertex IDs remain stable when cache configurations change. Authoritative data ownership is separated from optional cache residency. BFS chooses eligible backends based on placement and traversal capabilities, while request scheduling bounds pressure on slower stores. GPU projections can be prepared offline and loaded into cuGraph; the standard runtime retains process-local GPUs rather than pretending the MPI prototype is integrated.

## Engineering highlights

| Area | In the repository |
|:--|:--|
| **Execution** | BFS coordinator, batch expansion, direction/filter specifications, native backend pushdown where supported |
| **Data placement** | Stable logical IDs, backend-aware placement catalog, backend routing and external GPU/cache residency |
| **Storage** | Neo4j, ArangoDB, PostgreSQL adapters; Redis and memory-mapped adjacency caches |
| **Acceleration** | Optional CUDA/cuGraph execution, dense GPU projections, multi-GPU sharding in a single process |
| **Performance work** | Configurable backend scheduling, request batching, event traces, benchmark matrix tooling |
| **Operations** | HTTP endpoint, structured events, Prometheus/Grafana integration, diagnostic scripts |
| **Reproducibility** | LDBC SNB preparation, storage placement configurations, repeatable benchmark workflows |

## Try it

**Prerequisites:** CMake ≥ 3.27, Ninja, C++23 compiler, Python 3, and the C++ dependencies selected by your build. Docker Compose and locally available graph data are required for end-to-end backend traversal. GPU builds additionally need a compatible CUDA/RAPIDS/cuGraph installation.

```bash
# Build the CPU traversal application
cmake --preset cpu-bfs
cmake --build --preset cpu-bfs

# Discover CLI options; a populated domain is required to run a traversal
./build/cpu-bfs/bin/polygraph_bfs --help

# Read the step-by-step data workflow
# docs/guides/GETTING_STARTED.md
```

The [first-run guide](docs/guides/GETTING_STARTED.md) covers acquiring an LDBC dataset, preparing its domain, loading persistent stores, creating independent cache plans, executing BFS, and analyzing events. [Build options](docs/guides/BUILDING.md) explain CPU, GPU, and HTTP presets.

## Explore the internals

| Start here | What you'll learn |
|:--|:--|
| [Architecture](docs/architecture/ARCHITECTURE.md) | Request flow, backend abstraction, ownership and GPU projection construction |
| [Caching and residency](docs/architecture/CACHING.md) | Why immutable graph data and independently selected cache plans matter |
| [Backend request scheduling](docs/architecture/BACKEND_REQUEST_SCHEDULING.md) | Concurrency limits and backpressure around heterogeneous databases |
| [API reference](docs/reference/API_REFERENCE.md) | Core C++ classes, contracts, and links to the generated API |
| [GPU experiments](docs/research/GPU_PERFORMANCE_EXPERIMENTS.md) | Performance hypotheses, trade-offs, and paths explored |
| [Testing](docs/guides/TESTING.md) | Verification workflow and what needs external infrastructure |

## Research status and boundaries

PolyGraph is an **active research codebase**. CPU BFS and multi-store operation are the primary runtime path; GPU functionality is optional and toolchain-dependent. Standalone native multi-GPU cuGraph and CUDA-aware MPI/RDMA probes are now organized under [`src/gpu/distributed/`](src/gpu/distributed/README.md), but **are not a distributed production graph backend**. Running them requires an MPI-enabled GPU environment; CUDA-aware MPI alone does not prove GPUDirect RDMA. See the [integration status and promotion gates](docs/research/DISTRIBUTED_GPU.md).

Published benchmark notes are [collected here](docs/benchmarks/SF1_DISTRIBUTION_MATRIX.md). They document specific workloads and machines, **not universal speedup claims**. No CI badge is displayed without a verifiable hosted CI pipeline. The project provides a `cpu-tests` preset for GoogleTest and CTest, alongside an independent raw-CSV BFS oracle; see the [testing guide](docs/guides/TESTING.md) for the validation workflow.

## Developer documentation

Generate a searchable Doxygen wiki with all Markdown guides and C++ API symbols:

```bash
cmake --preset docs
cmake --build --preset docs
# Open build/docs/docs/generated/html/index.html
```

The docs-only preset avoids CUDA, database, and application dependencies. Open the [wiki index](docs/README.md) for a guided reading order.

---

**Tech:** C++23 · CMake/Ninja · Python · CUDA/cuGraph (optional) · Neo4j · ArangoDB · PostgreSQL · Redis · Drogon HTTP · Prometheus/Grafana
