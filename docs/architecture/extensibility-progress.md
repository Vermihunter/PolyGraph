# Graph extensibility progress: issues #28–#38

This work extends PolyGraph's existing ingestion engine. Dataset readers map
source data into typed logical records; shared normalization, placement, storage
writers and runtime traversal consume those records and graph metadata.

## Completed work

| Issue | Commit | Result |
| --- | --- | --- |
| #28 | `867d1f9` | Audited actual C++, Python, CMake, configuration, database, cache, partition and test coupling. Classified adapter, core, backend and already-reusable responsibilities. |
| #29 | `238c2a3` | Specified logical graph, source identity and input adapter contracts, extension boundaries and compatibility constraints. |
| #30 | `2ceb244` | Added explicit statically linked adapter registration and discovery while preserving built-in LDBC SNB and JSONL entry points. |
| #31 | `6c351f9` | Added graph manifests and read-only native preflight with manifest-relative path resolution and actionable configuration checks. |
| #32 | `06d1381` | Added exact typed source identities and bounded SQLite-backed ID/endpoint resolution, including reused IDs across logical types and entity kinds. |
| #33 | `d8537f8` | Added a shared native manifest import workflow for preparation, strict verification and opt-in execution through existing executors. |
| #34 | `2408b05` | Made runtime roots resolve from graph source metadata and typed source selectors, preserving numeric roots and BFS behavior. |
| #35 | `e091cde` | Consolidated Python graph/config selection, generic tools and measurement handling; retained legacy compatibility commands and isolated LDBC helpers. |
| #36 | `60cace9` | Unified logical/physical schema-name mapping at Neo4j/ArangoDB boundaries, scoped Arango graph-wide reads, and added unfamiliar-schema placement/cache/storage regressions. |
| #37 | `fe22ac1` | Added deterministic graph workspaces, shared native/Python path resolution, ownership/collision checks and guarded overwrite/cleanup paths. |
| #38 | This change | Added a typed movie CSV adapter, linked example application, content-derived import generations and an independent 1–3-hop reachability oracle. Exposed the existing CLI through an application-owned registry. |

## Resulting extension boundary

1. An application registers a linked reader through `InputAdapterRegistry`.
2. The reader streams nodes and relationships with logical types, supported
   properties and canonical or typed external identities.
3. The existing SQLite normalization engine validates identities and endpoint
   closure, allocates canonical IDs, applies placement and derives proxies/replicas.
4. Existing backend writers emit Neo4j, ArangoDB and PostgreSQL artifacts.
5. Graph manifests and workspace metadata select the schema, source index, runtime
   paths and generated artifacts. Runtime roots and traversal use the logical graph.

Backend-specific labels, collections, SQL representation and transport remain in
backend adapters. Cache and partition code continue to operate on generic topology.
No parallel ingestion framework, deployment manager, multitenant runtime or
PostgreSQL relational-schema redesign was introduced.

The movie example exposed one remaining composition gap: a custom engine registry
could not reach the hardwired CLI. The CLI implementation is now a reusable
`polygraph::ingestion_cli` target. Its stock executable retains the built-in
registry, while the example registers its own reader locally. Dataset-specific
movie behavior stays under `examples/movie-graph`.

## Compatibility and verification

LDBC SNB, canonical JSONL, numeric roots, typed source roots, all three native
backend formats and existing BFS contracts remain covered by regression tests.
Immutable generations and conservative ownership checks prevent one graph from
silently replacing another's artifacts. Existing explicit paths remain supported;
unknown legacy Python output directories require fresh destinations rather than
being adopted or deleted.

At completion of #38:

- Full CPU build passed.
- CTest: **595 passed**, with **3 optional/live skips** (598 registered).
- Python: **400 passed**, plus **3,331 subtests passed**.
- The documented movie onboarding command prepared and strictly verified
  **9 nodes and 9 relationships**.
- Docs build, local links, C++/new Python formatting and Python syntax checks passed.
- Independent review reported no actionable findings.

During #36, isolated live imports and reference BFS passed for both the unfamiliar
schema and LDBC SNB on Neo4j 5.26, ArangoDB 3.12 and PostgreSQL 17. Modified runtime
visitor reads, insertion and filtered fast traversal paths were also exercised.
Those live checks were not repeated for the later workspace/example changes.
Movie live onboarding and CUDA execution have not been claimed or performed here.

## Remaining planned work

- #39: adapter and cross-dataset contract/error-path tests.
- #40: isolated three-backend example onboarding and BFS smoke tests.
- #41: complete onboarding and extension documentation.

These issues have not been started automatically.

## Detailed references

- [Original coupling audit](graph-extensibility-audit.md)
- [Logical graph and adapter contracts](logical-graph-contract.md)
- [Schema-agnostic storage audit](schema-agnostic-storage.md)
- [Graph manifests and shared import workflow](../guides/GRAPH_MANIFESTS.md)
- [Graph workspaces and migration](../guides/GRAPH_WORKSPACES.md)
- [Movie example, command, fixture and oracle](../../examples/movie-graph/README.md)
