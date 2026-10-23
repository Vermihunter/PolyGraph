# PolyGraph documentation

PolyGraph is a heterogeneous graph runtime that routes one logical graph across persistent stores and acceleration layers such as Neo4j, ArangoDB, PostgreSQL, GPU/cuGraph, Redis, and memory-mapped adjacency caches.

## Start here

- [GETTING_STARTED.md](GETTING_STARTED.md) — clone, build, obtain LDBC SNB data, prepare it, load persistent backends, build caches, run BFS, and reconstruct events.
- [SCRIPTS.md](SCRIPTS.md) — practical command reference for `tools/polygraph.py` and the benchmarking/cache utilities.
- [ARCHITECTURE.md](ARCHITECTURE.md) — runtime components and the canonical-ID + external-residency routing model.
- [CACHING.md](CACHING.md) — reusable `cache-plan.sqlite3`, independent cache plans, Redis materialization, GPU residency, and overlap policy.
- [BENCHMARKING.md](BENCHMARKING.md) — BFS and multi-cache benchmarking workflows.
- [OBSERVABILITY.md](OBSERVABILITY.md) — operational logs, structured events, metrics, Prometheus/Grafana, and reconstruction.
- [CONFIGURATION.md](CONFIGURATION.md) — domain, backend, cache, and observability configuration.
- [BUILDING.md](BUILDING.md) — CMake presets and build variants.
- [INSERTION.md](INSERTION.md) — graph IDs, data ownership, and loading.
- [TESTING.md](TESTING.md), [STATIC_ANALYSIS.md](STATIC_ANALYSIS.md), and [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
- [API_CONVENTIONS.md](API_CONVENTIONS.md) and [API_REFERENCE.md](API_REFERENCE.md) — C++ API documentation.

`docs/benchmarks/` is intentionally kept as a separate thesis/benchmark document tree and is not modified by the documentation consolidation.
