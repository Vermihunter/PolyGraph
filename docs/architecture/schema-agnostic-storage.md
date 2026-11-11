# Schema-agnostic storage, placement and caches (#36)

Parent: #26. This audit follows the [initial architecture audit](graph-extensibility-audit.md). It checks the current ingestion engine and runtime, rather than introducing another ingestion abstraction.

## Confirmed coupling and fixes

`ingestion::sanitize_name` (`src/ingestion/Config.cpp`) and `tools.graphio.domain.sanitize_name` already map logical names to ASCII storage names. `src/ingestion/Writers.cpp` uses these names for Neo4j labels/relationship types and ArangoDB collections, while retaining logical names in properties and proxy `remote-type` metadata. Punctuation and Unicode make those representations differ. LDBC's ordinary ASCII names concealed runtime code treating them as identical.

`utilities/StorageNames.hpp::storage_type_name` now holds the unchanged C++ spelling rule. The ingestion wrapper preserves its existing error type. `data_access/adapters/StorageTypeNames.hpp` builds bounded per-schema forward/reverse maps once, rather than scanning types per returned node. It accepts logical names and physical aliases when decoding responses. Ambiguous names fail explicitly. Node and edge namespaces stay separate for Neo4j; ArangoDB additionally checks its shared collection namespace.

| Boundary | Concrete implementation | Classification and action |
| --- | --- | --- |
| Neo4j query labels and relationship types | `Neo4JQueryVisitor::Impl::visit(InsertBatchQuery)`, `edge_type_names_for_spec`, `physical_source_label_for_neo4j`, `group_frontier_by_physical_source_label` | Backend mapping: translate type IDs at physical query boundaries. |
| Neo4j response types | `LocallyDeployedNeo4JResponseProcessor::Impl::json_to_node`, `json_to_edge`; visitor rich/minimal response conversion | Backend mapping: recover logical type IDs from physical names; retain logical proxy metadata. |
| ArangoDB collection access and insertion | `ArangoQueryVisitor::Impl::visit(GetVertexByIdQuery)`, `visit(InsertBatchQuery)`, `visit(GetNeighborsBatchQuery)`, `edge_types_for_query`, `edge_index_scans_for_source_collection`, `physical_vertex_collection_for` | Backend mapping: use physical collection names for both queries and endpoint references. |
| ArangoDB document conversion | `ArangoQueryVisitor::Impl::json_to_node`, `json_to_edge`, minimal/candidate response conversion | Backend mapping: decode collection names through the schema mapping. |
| ArangoDB graph-wide reads | `ArangoQueryVisitor::Impl::construct_all_query` | Generic graph scope implemented in the backend: read declared node/edge collections and `Proxy`, excluding ingestion lock and unrelated database collections. Reproduced against the live fixture before the fix. |

Logical target filters comparing `type` or `remote-type` properties retain original schema names. `Proxy`, `POLYGRAPHVertex`, `localId`, `_key`, `_from` and `_to` are deliberate backend protocol details, not dataset entities.

## Existing reusable paths retained

| Area | Actual implementation | Decision |
| --- | --- | --- |
| Dataset parsing | `src/ingestion/Input.cpp` LDBC adapter, `LdbcSpec.hpp`; `tools/ldbc_snb` | LDBC filenames, columns and block IDs belong here. JSONL remains a separate existing adapter. No dataset parsing moves into core. |
| Normalization and identity | `src/ingestion/Normalize.cpp`, `SourceIdentity.cpp`, SQLite staging | Generic canonical records and exact identities; unchanged. |
| Domain configuration | `tools/graphio/domain.py::resolve_domain`, `resolve_graph_manifest` | Schema, paths, owner routing and source metadata are explicit. Legacy LDBC defaults are compatibility entry points, not cache or placement requirements. |
| Placement and proxies | `src/ingestion/PartitionStage.cpp`, `Writers.cpp`; runtime `PlacementCatalog` and `BackendEncodedUuid` | Generic routing and endpoint ownership. External partition keys are adapter-resolved identities (canonical JSONL uses decimal GlobalIds); no LDBC columns are needed. Proxies retain remote logical type and canonical endpoint identity. |
| Partition CSR | `PartitionStage.cpp::project`, `Kaminpar.cpp` | Generic dense topology, simple undirected deduplication and self-loop removal for partitioning only. Original relationships remain available to writers/BFS. No refactor. |
| Cache selection and adjacency | `tools/graphio/cache_planner.py::plan_graph_caches` and its SQLite/artifact builders | Generic IDs, degrees, optional logical-type policy filters and residency masks. mmap/Redis are existing unfiltered bidirectional neighbor caches. No refactor or change to filtering semantics. |
| GPU projection and placement | `cache_planner.py::_build_gpu_projection_artifact`; `gpu_storage_planner.py::plan_gpu_storage` | Generic canonical IDs, dense vertex mapping, adjacency arrays and device budgets. Artifact generation works without GPU hardware. No GPU algorithm change. |
| PostgreSQL | `src/ingestion/Postgres.cpp`, `Writers.cpp`; `src/data_access/adapters/PostgreSQL` | Generic normalized node/edge storage and JSON properties already retain unfamiliar logical names. Keep its backend-specific representation; no relational schema redesign. |
| Testing | `tests/ingestion_test.cpp`, `ingestion_partition_test.cpp`, Python graphio tests and reference BFS | Existing harnesses remain; add fixture regressions and an explicitly opted-in live visitor test. |

## Extension boundaries and compatibility

Add future dataset readers through the existing input adapter registry and normalization engine. Dataset readers supply logical names, arbitrary supported properties and source identities. Cache, partition, placement and BFS code consume normalized topology and graph metadata. Backend adapters own physical names and transport queries.

Existing safe ASCII names, GlobalIds, source identities, import formats and LDBC SNB behavior remain unchanged. No configuration migration is required. The mapping is constructed after schema initialization; adding types to a running visitor requires rebuilding that visitor, as schema/coverage initialization already precedes runtime construction. Mapping memory scales with schema size, not graph size.

Lossy name collisions (for example `Device-unit` and `Device_unit`) cannot be represented independently by the current ingestion format and are rejected. ArangoDB cannot share a collection between a node and edge type; Neo4j keeps separate namespaces. Existing ad hoc stores using raw punctuation-bearing labels from old runtime insertion paths may need reimport through the canonical ingestion engine. This change does not rename existing database objects or add a migration framework.

## Verification fixture

`tests/fixtures/schema_agnostic` contains four nodes (`Device-unit`, `Site/office`, `Gateway:router`, `Sensorµ`) with arbitrary properties, five relationship types, parallel edges, a self-loop, all three owners, and external partition assignments. Native strict verification checks emitted records/proxies. Cache tests compare emitted adjacency and GPU dense arrays to the input topology. Live import uses the existing disposable Docker workflow and its SQLite reference BFS; the opt-in visitor test covers physical reads, filtered rich expansion and runtime insertion.

To reproduce live checks with fresh owned resources:

```sh
python3 tools/ingestion/docker_load.py load \
  --binary build/cpu-tests/bin/polygraph_bulk_import \
  --adapter polygraph-jsonl --input tests/fixtures/schema_agnostic/graph.jsonl \
  --placement-config tests/fixtures/schema_agnostic/placement.json \
  --docker --keep-running --verify-bfs --bfs-binary build/cpu-tests/bin/polygraph_bfs \
  --output-dir build/schema-live-fresh
POLYGRAPH_SCHEMA_LIVE_ROOT="$PWD/build/schema-live-fresh" \
  ctest --test-dir build/cpu-tests -R SchemaBackendLive --output-on-failure
python3 tools/ingestion/docker_load.py stop --state build/schema-live-fresh/environment.json
```

The opt-in visitor test writes extra fixture nodes/edges; use only the disposable environment. Normal CTest runs skip that test. GPU artifact verification does not establish CUDA execution or GPU performance. Live Redis service behavior is outside this change; its portable cache artifacts are tested.

## Executed verification

- CPU build and full CTest: 585 registered, 582 passed, two optional KaMinPar tests and the opt-in live visitor test skipped. The live visitor test passed separately against the owned unfamiliar-schema environment.
- Full Python suite: 379 tests and 3,297 subtests passed.
- Fresh unfamiliar-schema Docker import: strict verification, live storage verification on Neo4j 5.26, ArangoDB 3.12 and PostgreSQL 17, and cross-backend reference BFS passed.
- Fresh LDBC SNB Docker import and cross-backend reference BFS passed on the same three backend versions.
- Direct live visitors: graph-wide reads, node lookup, node/edge insertion, Neo4j rich/minimal fast traversal (typed and unresolved roots), ArangoDB traversal/edge-index modes, proxy target type recovery and exclusion filters passed.
- Documentation build and local Markdown link checks passed.

CUDA execution and live Redis service behavior were not tested. Optional KaMinPar execution was not rerun for this change; external partition assignment and existing CPU partition tests ran.

The first additional LDBC live attempt failed during Neo4j startup because the disposable helper generated a password beginning with `-`, which Neo4j interpreted as an option. A fresh generation passed. The existing helper password-generation behavior was left outside this issue's scope; failed-generation journals were retained rather than reused. All owned test containers and networks were removed after verification.
