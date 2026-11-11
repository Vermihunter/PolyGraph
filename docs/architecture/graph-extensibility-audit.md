# Graph extensibility audit

Issue [#28](https://github.com/Vermihunter/PolyGraph/issues/28), inspected at
`78b4ffa` (2026-11-07). Documentation only; no production refactor.

PolyGraph already has the right ingestion engine to extend. Runtime traversal
is schema-driven, and backend writers consume normalized records. The principal
source leak is that normalization assigns **LDBC IDs to every ID-less record**.
Python additionally mixes placement, source IDs and orchestration in LDBC modules.
Extend the existing interfaces; do not add a parallel ingestion framework.

## Entry points and shared artifacts

| Path / symbol | Role and boundary |
| :-- | :-- |
| [app/bulk_import/main.cpp](../../app/bulk_import/main.cpp), `main`, `parse` | `prepare`, `verify`, `execute`, `verify-live`, Docker `load`/`verify-load`. Help exposes two input adapters. |
| [include/ingestion/Import.hpp](../../include/ingestion/Import.hpp), `Record`, `IGraphInputAdapter`, `PrepareOptions`, `IImportExecutor` | Existing streaming node/relationship passes, metadata, placement and native execution contracts. `Record.id` is optional, but generic ID-less input is not currently implemented. |
| [src/ingestion/Import.cpp](../../src/ingestion/Import.cpp), `prepare`, `verify`, `verify_semantics` | Input/configuration identity, resumable generation, strict reconstruction and atomic manifest publication. |
| [tools/ingestion/docker_load.py](../../tools/ingestion/docker_load.py), `bootstrap_runtime`, `prepare_runtime` | Fresh Docker lifecycle and conversion of the normalized index into runtime schema/metadata. Independent of LDBC labels. |
| [tools/polygraph.py](../../tools/polygraph.py), `prepare_university`, `prepare_ldbc`, `load_domain`, `reconfigure_cache` | Older domain preparation/loading/cache workflow. Separate from native ingestion; preserve both entry points. |
| [tools/graphio/reader.py](../../tools/graphio/reader.py), `GraphReader`, `open_graph_reader` | JSON, JSONL and lazy LDBC descriptor input for Python consumers. |
| [src/cli/DomainManifest.cpp](../../src/cli/DomainManifest.cpp), `load_domain_manifest`; [src/cli/GraphRuntimeBuilder.cpp](../../src/cli/GraphRuntimeBuilder.cpp), `build` | Runtime domain paths, logical schema, placement, configured stores and projections. BFS CLI and HTTP service share runtime facilities. |

Native preparation produces `manifest.json`, `generation-state.json`, a generation's
`index/graph.sqlite3`, checksummed backend chunks and optional partition artifacts;
execution adds journals/logs and live verification. The normalized index holds
nodes, edges, identities, replicas, proxies and partitions. These are ingestion
artifacts, not the runtime domain manifest.

Python domains use `domain.json` pointing to schema, backend config, graph data,
roots and optional cache/GPU plans. `polygraph.ldbc-csv` descriptors reconstruct raw
CSV lazily; they are not ordinary graph JSON. Cache plans reference mmap adjacency
(`index.bin`, `neighbors.bin`), GPU projection and residency artifacts. Native ID
maps (`polygraph.native-id-map`) bridge rewritten IDs in legacy workflows through
`tools/graphio/id_codec.py` and `src/cli/NativeNodeIdResolver.cpp`.

## Categorized findings

### Dataset-specific behavior: keep or move behind the input adapter

| Evidence | Finding / recommended boundary |
| :-- | :-- |
| [src/ingestion/LdbcSpec.hpp](../../src/ingestion/LdbcSpec.hpp), `node_specs`, `edge_specs`; [src/ingestion/Input.cpp](../../src/ingestion/Input.cpp), `LdbcAdapter::{nodes,relationships,load_aux}` | Eight node labels, 23 edge types, filename/shard recognition, Person email/language joins, `ldbc_id`, numeric-property rules and snapshot/update-stream exclusions belong to LDBC. Already substantially isolated; keep those semantics and ordering. `scalar`, `rows`, `properties` in this file implement CsvBasic conventions, not a universal CSV schema. |
| [src/ingestion/Normalize.cpp](../../src/ingestion/Normalize.cpp), `LdbcIds::{node,edge,metadata}`, `normalize` | **Primary leak.** `r.id ? *r.id : ids.node/edge(...)` makes every new ID-less adapter require numeric `Type:id` keys and the SNB edge catalog. Move source payload assignment behind an explicit adapter-associated policy that receives the selected backend; keep canonical validation, endpoint lookup and proxy planning in normalization. Assigning final IDs before partition-based owner selection would be incorrect. |
| [src/ingestion/Input.cpp](../../src/ingestion/Input.cpp), `make_adapter`, `input_identity`; [src/ingestion/Import.cpp](../../src/ingestion/Import.cpp), `prepare` | Factory and file inventory explicitly branch on the two adapter names; manifest `id_strategy` is a ternary on `ldbc-snb`. Extend this existing dispatch with adapter-owned inventory/strategy metadata, rather than teaching shared orchestration every dataset's file layout. Retain engine-owned hashing and publication. |
| [tools/ldbc_snb/spec.py](../../tools/ldbc_snb/spec.py); [tools/ldbc_snb/convert.py](../../tools/ldbc_snb/convert.py), `Placement.node_route`, `iter_nodes`, `iter_relationships`, `write_schema` | Python `Placement` combines reusable owner selection with block31 source identity allocation, numeric external IDs and SNB type ordering. Separate these responsibilities only when extending it; retain source allocation in LDBC. C++ and Python duplicate the catalog/ID algorithm, so parity is a compatibility contract. |
| [tools/ldbc_snb/fast_prepare.py](../../tools/ldbc_snb/fast_prepare.py), `prepare`; [tools/ldbc_snb/raw_reader.py](../../tools/ldbc_snb/raw_reader.py), `LdbcCsvGraphReader`; `prepare_schema.py`, `statistics.py`, `select_bfs_roots.py`, `placement_report.py`, `optimize_proxy_placement.py` in the same directory | Deliberately SNB-specific preparation, statistics, schema/root defaults and placement studies. Keep them as dataset tooling; do not relocate wholesale into core. Fast preparation has explicit schema-placement/no-cache shortcuts, not a generic ingestion replacement. |
| [tools/algorithms/bfs.py](../../tools/algorithms/bfs.py), `Graph`, `load_graph`, `placement_from_schema` and raw CSV loading helpers | Adjacency BFS and graph-reader loading are reusable; raw LDBC reconstruction imports `Placement`, `NODE_SPECS`, `EDGE_SPECS` directly. Keep raw parsing/identity in a source helper. Preserve the separate independent oracle under `tests/reference`. |
| [tools/generators/sample-generator.py](../../tools/generators/sample-generator.py), `generate_value_for_property`; [tools/polygraph.py](../../tools/polygraph.py), `prepare_university` | University sample names, email domains and type-specific values are another dataset adapter/example, not universal generation semantics. `tools/graphio/domain.py::generate_sample_graph` already provides schema-driven sample generation; extend that instead of inventing another generator. |

### Generic behavior: belongs in core, with small coupling fixes

| Evidence | Finding / recommended boundary |
| :-- | :-- |
| [tools/graphio/domain.py](../../tools/graphio/domain.py), `write_roots_csv`, `generate_sample_graph` | Generic root CSV exports `external_id` only from `properties.ldbc_id`; generic generated metadata defaults to `University`. Introduce explicit external-ID provenance and a neutral/schema-derived domain fallback additively. Do not silently change the roots CSV columns. |
| [src/cli/BackendConfigParser.cpp](../../src/cli/BackendConfigParser.cpp), default Arango configuration near line 336; [tools/graphio/loaders.py](../../tools/graphio/loaders.py), `ArangoHttpAdapter.__init__`; [tools/load_graph.py](../../tools/load_graph.py), `main`; [tools/measurement/graph_bench/config.py](../../tools/measurement/graph_bench/config.py), `ArangoConfig` | Implicit `university` database names leak a demo into reusable entry points. Connection selection belongs to configuration, distinct from Arango protocol handling. Keep legacy defaults during an additive migration; require explicit database selection for new domain creation. `tools/generators/from-file-to-db.py` repeats this default. |
| [tools/polygraph.py](../../tools/polygraph.py), `prepare_ldbc`, `prepare_university`, `_rewrite_backend_cache_config`, CLI parser | Named dataset commands are valid conveniences. Shared preparation has no equivalent arbitrary-dataset command; cache rewrite also falls back to `university`, and LDBC root options default to `Person`. Keep named presets, expose generic manifest/graph inputs through existing domain helpers. |
| [tools/workflows/common.py](../../tools/workflows/common.py), `canonical_sf`, `domain_dir`, `raw_dir`; `use_sf.py`, `reinitialize_sf.py`; [tools/cache/build_benchmark_plans.py](../../tools/cache/build_benchmark_plans.py), `main`; [tools/storage/reset.py](../../tools/storage/reset.py), `main` | SF paths and schema placement are built around `data/ldbc-snb`; reset-all marker cleanup scans only that tree. These are LDBC workflows presented under broad tool names. Keep SF conveniences explicitly scoped; generic lifecycle/cache operations should take a domain manifest, not infer one from scale factor. Preserve ownership checks and destructive-action safeguards. |
| [tools/graphio/reader.py](../../tools/graphio/reader.py), `open_graph_reader`; [tools/ingestion/docker_load.py](../../tools/ingestion/docker_load.py), parser; [app/bulk_import/main.cpp](../../app/bulk_import/main.cpp), help | Finite dispatch/choices are extension sites, not evidence that a plugin framework is missing. Add new adapters here when needed. `tools/polygraph_gui.py` delegates to existing tooling; no additional hardcoded SNB type catalog was found there. |

### Backend-specific behavior: retain in backend adapters

| Evidence | Behavior to retain |
| :-- | :-- |
| [src/ingestion/Writers.cpp](../../src/ingestion/Writers.cpp), `postgres`, `arango`, `neo4j`, `property_type`, `array_fixups`, `generate_backend` | COPY rows, collection/label sanitization, typed Neo4j headers and property repairs; Arango document keys and endpoint handles. These are physical storage constraints, not LDBC coupling. |
| [src/ingestion/Postgres.cpp](../../src/ingestion/Postgres.cpp), `postgres_executor`; [Arango.cpp](../../src/ingestion/Arango.cpp), `arango_executor`; [Neo4j.cpp](../../src/ingestion/Neo4j.cpp), `neo4j_executor` | Native import transport, fresh-target validation, commands and exact live checks stay backend-specific behind `IImportExecutor`. |
| [storage_layer/postgresql/init/001_polygraph.sql](../../storage_layer/postgresql/init/001_polygraph.sql); [src/data_access/adapters](../../src/data_access/adapters); [tools/graphio/loaders.py](../../tools/graphio/loaders.py), `Neo4jHttpAdapter`, `ArangoHttpAdapter`, `PostgreSqlAdapter` | Generic PostgreSQL node/edge tables and JSONB properties already accept arbitrary types. Query visitors/stores translate schema types into backend queries. PostgreSQL proxies retain logical types; Neo4j/Arango use `Proxy`. Preserve this distinction. |
| [src/ingestion/CMakeLists.txt](../../src/ingestion/CMakeLists.txt); [src/cmake/Backends.cmake](../../src/cmake/Backends.cmake); [cmake/KaMinPar.cmake](../../cmake/KaMinPar.cmake) | SQL embedding and optional libpq/curl/KaMinPar dependencies are backend/build capabilities. No dataset is selected by CMake. Standalone ingestion is already separated from BFS/GPU/server targets. |
| [storage_layer](../../storage_layer), Compose files and `arango/*.sh`, `neo4j/*.sh`; [config/backends](../../config/backends) | Image/volume/service wiring and database inspection are backend operations. University URLs in example configs are presets; the same URL in an implicit loader default is the coupling identified above. SQL traversal indexes are a separate optional benchmark choice. |

### Existing reusable functionality: no dataset refactor needed

| Evidence | Why retain it |
| :-- | :-- |
| [src/ingestion/Normalize.cpp](../../src/ingestion/Normalize.cpp), `normalize` outside `LdbcIds`; [Config.cpp](../../src/ingestion/Config.cpp), `Placement::route`; [Storage.cpp](../../src/ingestion/Storage.cpp) | Bounded SQLite indexing, identity uniqueness, endpoint closure, independent edge ownership, replicas/proxies, property validation, locks and atomic files operate on arbitrary record types. Physical reserved names are explicit constraints. |
| [src/ingestion/PartitionStage.cpp](../../src/ingestion/PartitionStage.cpp), `project`, `prepare_partition_assignments`, `finish_partition_metadata`; [Partition.cpp](../../src/ingestion/Partition.cpp), [Kaminpar.cpp](../../src/ingestion/Kaminpar.cpp) | Topology uses adapter source keys, not SNB labels. Simple undirected unit-weight CSR collapses parallel/directional edges and omits self-loops only for partitioning; original import records remain intact. External assignments and partition-to-backend maps are already extension points. |
| [include/domain/graph/LogicalGraphSchema.hpp](../../include/domain/graph/LogicalGraphSchema.hpp), `add_node_type`, `add_edge_type`; [src/algorithms/bfs/BfsExecutor.cpp](../../src/algorithms/bfs/BfsExecutor.cpp); [src/algorithms/common/NeighborExpander.cpp](../../src/algorithms/common/NeighborExpander.cpp); [src/infrastructure](../../src/infrastructure); [src/insertion](../../src/insertion) | Runtime types, expansion filters, placement catalogs, scheduling and insertion policies use schema/type/backend IDs. No SNB label catalog was found in runtime C++ outside ingestion. |
| [tools/graphio/cache_planner.py](../../tools/graphio/cache_planner.py), `plan_graph_caches`; [gpu_storage_planner.py](../../tools/graphio/gpu_storage_planner.py), `plan_gpu_storage`; [tools/cache/load_redis.py](../../tools/cache/load_redis.py), `load` | Graph-reader inputs, topology policies, binary adjacency/projection and residency artifacts are reusable. `ldbc-cache*`, SF GPU and benchmark JSON files in `config/` are dataset experiments, not core requirements. |
| [src/data_access/adapters/cache](../../src/data_access/adapters/cache); [include/gpu/data_model](../../include/gpu/data_model); [tools/measurement/graph_bench](../../tools/measurement/graph_bench), `GraphData.load`, `relationship_filter_from_schema` | Cache stores, dense ID mapping, CSR projection and benchmark graph/schema handling do not need SNB names. Keep binary/transport implementation in its existing layer. |
| [src/cmake/Core.cmake](../../src/cmake/Core.cmake), [Model.cmake](../../src/cmake/Model.cmake), [Runtime.cmake](../../src/cmake/Runtime.cmake); [CMakePresets.json](../../CMakePresets.json) | Model/core/runtime/backend/ingestion targets and CPU/GPU capabilities already provide build boundaries. Do not introduce per-dataset runtime targets. |

## Deliberately unsupported or narrower contracts

- Native input currently supports only CsvBasic SNB snapshots and canonical JSONL;
  arbitrary CSV schemas, Parquet and SNB update-stream replay need adapters/semantics.
- Native `Backend` supports Neo4j, ArangoDB and PostgreSQL; core/Python ID codecs
  describing more locations do not imply native import support. Native placement
  rejects Python identity-hash/attribute-hash/label-striped strategies.
- Canonical JSONL requires IDs and matching `idRouting`, with 16 location bits and
  48 payload bits. Legacy benchmark single-backend zero-routing metadata is not
  interchangeable with this native input contract.
- `LogicalGraphSchema::add_edge_type` describes one source/target type pair per
  edge type. `docker_load.py::bootstrap_runtime` rejects multiple pairs for one
  name, while `prepare_runtime` can allow native loading without a BFS schema.
  This is a typed runtime limitation, not an LDBC parsing defect.
- mmap/Redis `supports_cache_spec` accepts unfiltered, bidirectional adjacency
  without edge refs. Typed/directed requests require capable stores/fallback;
  do not claim generic directed cache acceleration from successful undirected BFS.
- Native Neo4j rejects object/nested/heterogeneous array properties; backend
  differences and reserved fields remain enforced. KaMinPar is opt-in, uses
  32-bit vertex indices and has documented memory/determinism limits.

## Minimal non-LDBC happy path

No new adapter is needed for an already canonical graph. Write `graph.jsonl`:

```jsonl
{"kind":"metadata","idRouting":["Neo4J"],"idRoutingBits":16,"idPayloadBits":48}
{"kind":"node","uuid":281474976710657,"type":"Device","source":"Neo4J","properties":{"serial":"A"}}
{"kind":"node","uuid":281474976710658,"type":"Device","source":"Neo4J","properties":{"serial":"B"}}
{"kind":"relationship","uuid":281474976710659,"type":"LINKS","source":"Neo4J","from":281474976710657,"to":281474976710658,"properties":{}}
```

Use placement `{"storageBackends":["Neo4J"],"defaultNodeSource":"Neo4J","defaultRelationshipSource":"Neo4J"}`:

```bash
build/cpu-tests/bin/polygraph_bulk_import prepare --adapter polygraph-jsonl \
  --input graph.jsonl --placement-config placement.json --output-dir prepared
build/cpu-tests/bin/polygraph_bulk_import verify --manifest prepared/manifest.json --strict
```

Use a fresh output directory outside the input. This exercises arbitrary labels,
endpoint closure and native artifact generation offline. For live traversal, the
existing `load --docker --keep-running --verify-bfs` path can create fresh services
and derive a Device/LINKS runtime schema; or provide explicit targets to `execute`.
That live step requires Docker/backends and was not run for this audit. Python
`open_graph_reader`, `validate_graph`, domain helpers and cache planners already
accept this graph; full raw-source adapters are needed only before canonicalization.

## Follow-up decisions and compatibility risks

1. **First: isolate ID policy and inventory through the existing ingestion seam.**
   Choose an adapter-associated payload allocator receiving the selected owner,
   explicit strategy metadata and source file inventory. Preserve the existing
   JSONL path and LDBC allocator as defaults; specify stable IDs for a future
   string-key source before adding it. Keep partition/placement ordering intact.
2. **Preserve reproducibility before extracting Python placement.** Block31 prefix
   encounter order, edge catalog order and per-type/per-owner sequences affect
   persisted IDs, roots, proxy references and output parity. Hash strategies are
   not equivalent to C++ schema placement. Do not silently unify them.
3. **Version shared artifacts deliberately.** Changes to adapter identity,
   allocator/version metadata or ordering must invalidate preparation checkpoints
   and signatures. Coordinate manifest readers, native ID maps, domain paths,
   cache graph signatures and residency; old artifacts must remain readable or
   fail explicitly, never be reused under a new identity algorithm.
4. **Make generic provenance/configuration additive.** Decide an external-ID
   representation and explicit database/domain inputs; retain `ldbc_id`, roots
   CSV columns, named SF workflows, CLI names and University presets during migration.
5. **Keep schema widening separate.** Decide whether heterogeneous endpoint types
   should be supported by runtime schema or rejected at the runtime boundary.
   Do not narrow already-working native ingestion just to match BFS bootstrap.
6. **Validate the seam with a second dataset.** Add a small non-LDBC canonical
   fixture first, then an adapter only if raw input requires one. Cover source
   keys/string IDs, isolated vertices, parallel/directed/self edges, properties,
   partitions, cross-backend proxies and filtered traversal. Reuse current test
   stores, executors and oracle; no new testing framework is needed.

## Existing tests and audit validation

[tests/CMakeLists.txt](../../tests/CMakeLists.txt) registers ingestion C++ unit
tests, Python semantic/Docker workflow checks, independent BFS oracle and
opt-in live tests for both input adapters. `ingestion_test.cpp` and
`tools/ingestion/test_semantics.py` emphasize SNB/JSONL parity, Unicode, ID bounds,
closure, recovery and proxies; JSONL variants often retain Person/Post/Place names.
`ingestion_partition_test.cpp` covers topology and optional KaMinPar contracts.
`tests/support/ScriptedGraphStore.*`, BFS contract/concurrency tests and generated
executor fixtures already test generic runtime behavior without databases.
`tests/reference/{schema,ldbc_csv,dataset}.py` and the SF0.1 lock/query manifests
are intentionally independent SNB oracle inputs, not production reuse targets.
`tools/graphio/test_*` cover generic loaders, domain manifests, caches and GPU IDs.

No `AGENTS.md` was found in the repository source inventory or the second
workspace root. Generated build/database trees are not audit source inputs.

Checks run for this documentation-only change:

| Command / check | Result |
| :-- | :-- |
| `cmake --build --preset cpu-tests --target polygraph_bulk_import polygraph_ingestion_test polygraph_ingestion_partition_test -j 2` | Passed; rebuilt the importer and two ingestion test targets. |
| `ctest --test-dir build/cpu-tests -R 'unit.IngestionTest\|unit.IngestionPartitionTest\|python.ingestion_semantics\|python.reference_oracle' --output-on-failure` | 42/42 passed. The build has KaMinPar disabled; its unavailable-capability test ran, not real partitioning. |
| `python3 -m pytest -q tools/graphio/test_domain_manifest.py tools/graphio/test_loaders.py tools/graphio/test_cache_planner.py tools/graphio/test_gpu_uuid_routing.py tests/test_ldbc_snb_conversion.py tests/test_reference_bfs_ldbc_csv.py` | 57 passed. An earlier `unittest` invocation of these modules passed 31 tests but did not collect pytest functions; this pytest run covers both styles. |
| Device/LINKS example above, generated in a temporary directory | `prepare`, strict `verify`, counts (2 nodes/1 edge), preserved-ID strategy and Python `validate_graph` passed. No fixture or production code added. |
| `python3 tools/docs/check_links.py`; `git diff --check` | Passed. |
| `cmake --preset docs`; `cmake --build --preset docs` | Passed. Installed Doxygen warned that existing tags `MARKDOWN_ID_STYLE`, `HTML_COLORSTYLE`, `FULL_SIDEBAR` are unsupported. |

Live Docker/database imports, GPU execution, real KaMinPar and the full CTest
suite were not run. Findings about these paths are source inspection, not claims
of live validation. Additional production tests are follow-up work.
