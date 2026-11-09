# Relational PostgreSQL Implementation Plan

> **For agentic workers:** Use `superpowers:executing-plans` to implement approved tasks one at a time. Steps use checkboxes for tracking. This document is a proposed plan for review, not authorization to implement or a claim that design choices have been approved.

**Goal:** Deliver issue #12 by supporting existing typed PostgreSQL node tables, junction tables, and direct foreign keys while preserving native keys, BFS, placement, and cross-backend traversal.

**Architecture:** Add an explicit mapped relational mode alongside the existing generic PostgreSQL representation. A shared, validated mapping model drives both relational import and a relational graph store; external identities resolve through a durable graph-generation identity catalog. Keep application tables free of PolyGraph-specific columns and retain the backend-neutral traversal interface.

**Tech Stack:** Existing C++, libpq, SQLite prepared identity index, JSON configuration, GoogleTest/CTest, Python live-test tooling, PostgreSQL.

**Spec:** [Parent #12](https://github.com/Vermihunter/PolyGraph/issues/12) and child issues #13–#22 linked below; existing [logical graph contract](../../architecture/logical-graph-contract.md). Issue bodies and all issue discussions were checked on 2026-11-09; none had comments. This plan records proposed design decisions because the issues do not specify the physical mapping format or identity lifecycle.

## Global constraints

- Preserve application primary keys and native tables; no automatic alterations, renames, traversal indexes, or generic-table migration in relational mode.
- Keep `IGraphStore`, logical type IDs, existing placement/proxy behavior, and distributed BFS semantics compatible.
- Keep generic-table ingestion/runtime working; omitted relational configuration selects existing behavior.
- Use the existing typed source-identity encoding and canonical ID allocator. Do not cast arbitrary application keys to `GlobalId` or hash keys into IDs.
- Read and validate mappings before mutations. Quote identifier components separately and parameterize data values; mapping files do not contain arbitrary SQL expressions.
- Keep import and traversal bounded by batches. Preserve frontier joins and candidate streaming rather than issuing one database query per vertex.
- Do not create optional benchmark traversal indexes by default.
- Read applicable C++ and PostgreSQL skills before implementation; add testing/concurrency skills when the task requires them.

## Findings in the current tree

- `storage_layer/postgresql/init/001_polygraph.sql` defines generic `polygraph_nodes` / `polygraph_edges` with JSONB properties.
- `src/ingestion/Writers.cpp` emits that fixed CSV shape. `src/ingestion/Postgres.cpp` creates the generic schema, checks the whole database for unrelated relations, requires empty targets, COPYs data, and verifies generic rows. Its emptiness checks cannot be reused unchanged for application databases.
- `PostgreSqlGraphStore` COPYs a frontier into a temporary relation, builds generic-edge joins, and streams candidates with libpq single-row mode. Preserve this execution model in the new adapter.
- `domain::LogicalGraphSchema` describes logical node/edge types; physical SQL tables belong in a separate mapping model.
- `domain::SourceIdentity` already distinguishes namespace, logical type, and exact string/int64/uint64 values. `cli::SourceNodeIdResolver` performs read-only lookups in the prepared SQLite index. Reuse these contracts; prepared artifacts alone do not attach an arbitrary populated database to a graph.
- Backend construction passes through `BackendConfigParser`, `GraphRuntimeBuilder`, and `ConfiguredGraphStoreFactory`. Both import targets and runtime backend configuration need the same mapping reference and version.
- `IGraphStore::execute` is optional; PostgreSQL currently exposes traversal overrides. Typed property reading needs an explicit tested adapter API rather than assuming a working generic query visitor.

## Proposed choices and alternatives

**Recommended:** explicit mappings, a separate relational adapter, immutable identity/placement generation, and isolated PolyGraph metadata. This keeps application schemas intact and makes generic-mode regressions containable.

**Alternative:** replace the generic adapter/importer in place. This reduces adapter count but couples every existing benchmark and importer to the migration and conflicts with preserving current workflows.

**Alternative:** expose compatibility views named like generic tables. This can prototype reads, but still needs identity and proxy metadata, does not solve native typed writes, and can hide expensive per-row conversion. Do not use it as the primary architecture.

Proposed initial policy, subject to review:

1. Mapping format `polygraph.postgresql-relational`, version `1`, referenced by `relationalMapping` in a PostgreSQL backend/target. Resolve relative paths against the containing configuration file. Fingerprint mapping + logical schema + identity generation in prepared artifacts and reject runtime/import mismatches.
2. Each node mapping names logical type, SQL schema/table, primary-key column, source namespace, key codec, and logical-property-to-column mappings. Relationship mappings select `table` or `foreignKey`, name logical endpoint types, and map endpoint columns explicitly. Table relationships may map an optional unique relationship key and typed attributes.
3. Scalar keys first: smallint/integer/bigint as exact signed integers; text/varchar and UUID as strings with explicit SQL codecs. Normalize UUID text consistently. Reject composite keys with a specific diagnostic in the first slice; composite support is an explicit review decision, not an issue requirement already satisfied.
4. Map supported native property types explicitly to the existing logical property model. Preserve integer precision, SQL NULL, booleans, dates and timestamps; define timezone handling. Reject unsupported arrays, domains, enums, numeric precision, or other types unless an explicit lossless codec is implemented and tested. Do not silently stringify every column.
5. No table DDL for application objects. Read-only attachment uses existing rows plus an externally prepared identity catalog; runtime may use temporary tables but must not require writes to application tables. An optional writable PolyGraph metadata schema may accelerate identity/placement joins; provide a temp-table path for a role without persistent metadata privileges.
6. Identity generation is stable once published: bootstrap by deterministic typed-key ordering using the existing allocator; persist assignments and owner information. Later additions preserve previous IDs, removed IDs are not reused, and mapping/version mismatches fail. Stability across arbitrary independent reboots is not promised unless they reuse the same catalog. No full key dictionary in runtime RAM.
7. Relational import defaults to append into empty mapped destinations, tolerating unrelated tables. Reject existing conflicting keys; do not silently upsert or overwrite populated application tables. Attachment of populated databases is a separate read workflow. Updating pre-existing application rows requires a separately approved policy.
8. Existing direct FKs represent at most one target per owner row for each mapped column. Preserve NULL as no edge. Reject conflicting assignments instead of applying last-write-wins. Require declared FK constraints for FK mappings; junction endpoints may be logical references validated by metadata and data checks.
9. A local SQL FK cannot reference a remote-only row. Cross-backend relational references require an existing compatible local reference row or a dedicated PolyGraph metadata representation; never fabricate incomplete application rows to satisfy NOT NULL/FK constraints. Unsupported combinations fail preflight.
10. Attachment is a graph snapshot/generation. Concurrent out-of-band writes and automatic CDC/reconciliation are out of initial scope; detect dangling/unknown identities and require explicit refresh rather than silently inventing IDs during BFS.

## Review focus

- Same application key in different tables/namespaces must remain distinct; typed numeric and string keys must not collide (Tasks 1–2).
- Quoted names, non-public schemas, hostile identifiers, and search-path changes must select only configured objects (Tasks 1, 3–4).
- NULL FKs, duplicate junction rows, parallel edges, cycles and self-loops must preserve direction and BFS distance (Tasks 4–5, 7–8).
- Remote endpoint ownership must survive relational conversion and proxy expansion without writing dummy application rows (Tasks 2, 5, 7–8).
- Type errors, stale identity generations, and failed FK updates must fail explicitly and leave no partial import (Tasks 1–2, 6–8).

## Dependency and delivery order

| Issue | Deliverable | Depends on |
| --- | --- | --- |
| [#12](https://github.com/Vermihunter/PolyGraph/issues/12) | Parent architecture, compatibility, final acceptance | All children |
| [#13](https://github.com/Vermihunter/PolyGraph/issues/13) | Mapping model, configuration and catalog validation | Design decisions |
| [#15](https://github.com/Vermihunter/PolyGraph/issues/15) | Durable external-key identity and attachment | #13 |
| [#14](https://github.com/Vermihunter/PolyGraph/issues/14) | Typed node resolution and property reads | #13, #15 |
| [#16](https://github.com/Vermihunter/PolyGraph/issues/16) | Junction-table relationship reads | #13–#15 |
| [#17](https://github.com/Vermihunter/PolyGraph/issues/17) | Direct-FK relationship reads | #13–#15 |
| [#21](https://github.com/Vermihunter/PolyGraph/issues/21) | Relational adjacency wired into distributed BFS | #16, #17 |
| [#18](https://github.com/Vermihunter/PolyGraph/issues/18) | Typed node import | #13–#15 |
| [#19](https://github.com/Vermihunter/PolyGraph/issues/19) | Junction relationship import | #16, #18 |
| [#20](https://github.com/Vermihunter/PolyGraph/issues/20) | Direct-FK import | #17, #18 |
| [#22](https://github.com/Vermihunter/PolyGraph/issues/22) | Complete relational and mixed-backend acceptance | All above |

Introduce live fixtures with #13; grow them throughout the work. #22 consolidates coverage, rather than deferring integration until the end. Deliver the read-only slice through #21 before enabling relational writes.

## Task 1 — #13: Mapping contract and metadata validation

**Files:** Create `include/data_access/adapters/PostgreSQL/RelationalMapping.hpp`, `src/data_access/adapters/PostgreSQL/RelationalMapping.cpp`, `tests/postgres_relational_mapping_test.cpp`, `docs/architecture/postgresql-relational-mapping.md`, and `docs/examples/relational-postgresql/mapping.json`. Modify backend connection/config parser, `src/ingestion/Config.cpp`, runtime factory and relevant CMake source/test lists.

**Interfaces:** Define immutable `RelationalMapping`, `ValidatedRelationalMapping`, and `RelationalMappingValidator`; expose `parse_relational_mapping(const json &, const domain::LogicalGraphSchema &) -> RelationalMapping`. Validator consumes a libpq connection and mapping, produces validated table/column/type/constraint metadata shared by import and runtime. Keep SQL rendering internal.

- [x] Add parser tests for both relationship forms, duplicate logical mappings, unknown logical endpoints, unsupported versions, relative paths and exact identifier handling; run the focused test and confirm missing implementation fails.
- [x] Write the mapping contract and representative person/post/comment example using the proposed policy. Define required fields, key/property codecs, relationship orientation and allowed combinations before implementing parser types.
- [x] Implement parser and catalog validation using PostgreSQL object identities, not interpolated `search_path` lookups. Check keys, uniqueness, referenced endpoints, FK direction/nullability, column compatibility and privileges; distinguish read-only attachment and writable import validation.
- [x] Add disposable PostgreSQL tests: absent tables/columns, wrong schema, incompatible types, undeclared FK, ambiguous mapping, missing permissions, quoted/Unicode names and malicious configuration strings. Add an unrelated sentinel table and assert it is untouched.
- [x] Verify old backend/target files still parse unchanged; run focused mapping tests and existing config tests. Commit this independently reviewable contract.

## Task 2 — #15: Identity catalog and attaching existing rows

**Files:** Create `include/data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp`, corresponding implementation, `src/ingestion/PostgresRelationalAttach.cpp`, and `tests/postgres_relational_identity_test.cpp`. Modify ingestion storage/normalization/manifest and CLI plumbing only where needed; reuse `SourceIdentity.hpp`, `SourceNodeIdResolver`, and existing allocation code.

**Interfaces:** `RelationalIdentityResolver` consumes validated mapping + published identity generation. Provide bounded bidirectional lookup between `(namespace, logical type, typed external key)` and `(GlobalId, owner)`; attachment produces an immutable manifest/index compatible with runtime source-root lookup. Define generation publication and rollback behavior in the contract.

- [ ] Add failing tests for identical values in distinct types/namespaces, signed numeric versus string identity, UUID normalization, shuffled scan order, repeated attachment, missing identities and append preserving old IDs.
- [ ] Implement a streaming attachment/bootstrap command for existing mapped rows without importing or changing them. Persist mappings using the shared allocator/index; include physical SQL codec separately from logical source identity kind.
- [ ] Implement bounded reverse lookup and owner/partition checks for relational joins. Document whether temporary key/ID relations or optional metadata tables are used; never require a full in-memory identity map.
- [ ] Test generation mismatch, missing index, key deletion/reuse policy, stale mapping, rollback and restart. Compare source-root resolution between attachment and normal ingestion.
- [ ] Run focused identity tests and existing source identity/runtime tests; commit.

## Task 3 — #14: Typed node reads

**Files:** Create `PostgresRelationalNodeReader.hpp/.cpp` beside the mapping model and `tests/postgres_relational_node_test.cpp`; extend the relational contract with codec rules. Modify adapter CMake lists.

**Interfaces:** `PostgresRelationalNodeReader` consumes validated mapping + identity resolver and provides `read_nodes(std::span<const NodeRef>) -> std::vector<record_t>` using the existing record alias from `include/data_model/Record.hpp`. Verify the actual record construction API before coding; no new backend-neutral record hierarchy.

- [ ] Add failing batched property-read tests against person/post/comment tables in two SQL schemas.
- [ ] Implement identity-to-PK lookup and configured typed column projection. Explicitly preserve NULL, exact integer boundaries and date/timestamp semantics; report unsupported conversions with logical type and physical column.
- [ ] Test missing rows, unknown IDs, hostile identifiers, UUID/string keys, non-public schemas and an application schema with no PolyGraph columns.
- [ ] Verify no application DDL or writes occur; run focused reader tests and commit.

## Task 4 — #16 and #17: Relational adjacency query generation

**Files:** Create `RelationalAdjacencyQuery.hpp/.cpp` beside the mapping code and `tests/postgres_relational_adjacency_test.cpp`.

**Interfaces:** Query builder consumes `ValidatedRelationalMapping` and `ExpandSpec`; returns SQL and bound data needed to produce canonical source/target IDs and logical endpoint types. Frontier/identity relation names are adapter-owned. Both relationship forms expose the same canonical adjacency shape.

- [ ] Write failing direction/type-filter tests for junction tables, FK owner-to-reference and reverse joins. Include FK columns on either logical endpoint so SQL ownership does not silently determine graph direction.
- [ ] Implement only relevant per-mapping join branches combined with `UNION ALL`. Join typed external endpoints through identity relations; no numeric coercion or per-source SQL queries.
- [ ] For junctions, preserve optional attributes and declared relationship identity; do not collapse parallel edges in rich reads. Candidate deduplication remains controlled by existing BFS options.
- [ ] For FKs, omit NULL references; reject unknown non-NULL endpoints explicitly. Test nulls, self-loops, parallel rows, duplicate endpoints, empty filters and multiple endpoint types.
- [ ] Run the live adjacency tests for forward/reverse/both directions and commit the independently testable readers, with separate issue-specific commits where practical.

## Task 5 — #21: Runtime and distributed BFS integration

**Files:** Create `include/data_access/adapters/PostgreSQL/PostgresRelationalGraphStore.hpp`, corresponding source, and `tests/postgres_relational_bfs_test.cpp`. Modify `ConfiguredGraphStoreFactory`, backend config/runtime plumbing, CMake and existing backend live tests.

**Interfaces:** `PostgresRelationalGraphStore final : IGraphStore` implements `get_neighbors_batch`, `get_minimal_neighbors_batch`, and `stream_neighbor_candidates` with existing signatures. Consume Tasks 1–4; do not alter BFS executor contracts.

- [ ] Add failing end-to-end runtime tests selecting the relational adapter and resolving a source root from an attached database.
- [ ] Reuse the generic store's frontier COPY/transaction/streaming approach; extract shared libpq utilities only where necessary. Preserve filters, predecessor output, batch limits, candidate dedup options, cancellation/error draining and connection reuse after failure.
- [ ] Resolve placement and proxy adjacency through PolyGraph metadata, including edges whose canonical target owner is Neo4j/ArangoDB. Proxies remain metadata, not dummy rows in constrained application tables.
- [ ] Test rich/minimal/streamed path parity, empty frontier, both directions, cycles, depth limits, type filters, predecessor validity and cross-backend handoff. Compare exact expected distances with the generic path on the same logical fixture.
- [ ] Run relational BFS, existing BFS and generic PostgreSQL live regressions; commit. This completes the first read-only vertical slice.

## Task 6 — #18: Typed relational node ingestion

**Files:** Create `src/ingestion/PostgresRelational.cpp`, `src/ingestion/RelationalWriters.cpp`, and `tests/postgres_relational_ingestion_test.cpp`. Modify `Native.hpp`, executor selection, `Writers.cpp`, `Config.cpp`, artifact manifests/verification and ingestion CMake.

**Interfaces:** Add `postgres_relational_executor(ImportContext) -> std::unique_ptr<IImportExecutor>`. Relational artifacts identify destination mapping, explicit columns, native codecs, counts and identity generation; generic CSV artifacts remain unchanged.

- [ ] Write failing prepare/import/verify tests for typed rows and exact external PK preservation.
- [ ] Implement mapping-aware artifacts and batched COPY using explicit destination columns. Distinguish omitted defaults/generated columns from NULL; reject unrepresentable values before COPY.
- [ ] Replace database-wide emptiness checks only in relational mode with checks for mapped write destinations/conflicting keys. Validate required columns/defaults/identity-column policy; do not run generic `schema_sql`.
- [ ] Keep application writes and any PostgreSQL side metadata transactional. Publish the external catalog only after successful database commit; record interrupted publication so runtime rejects incomplete generations and recovery can complete publication safely.
- [ ] Test unrelated populated tables, missing required values, overflow, exact typed values, rollback and failed catalog publication. Reuse existing placement preparation and test owner/proxy counts separately from physical application row counts.
- [ ] Run relational import/verify and generic native import tests; commit.

## Task 7 — #19 and #20: Relationship ingestion

**Files:** Extend `PostgresRelational.cpp`, `RelationalWriters.cpp`, relational verification and `tests/postgres_relational_ingestion_test.cpp`.

**Interfaces:** Consume node mappings/catalog from Task 6. Junction artifacts contain external endpoint keys + configured typed attributes. FK artifacts contain owner key, referenced key or NULL and logical relationship identity for conflict detection.

- [ ] Add failing junction import tests covering external key resolution, optional attributes, parallel edges, self-loops and missing references.
- [ ] COPY junction rows after referenced nodes exist; honor existing constraints. Verify exact endpoint/type/attribute rows and preserve canonical edge identity in metadata where the application table has no edge key.
- [ ] Add failing FK tests for nullable no-edge state, repeated same assignment, conflicting targets, nonnullable columns, missing targets and references crossing backends without local reference rows.
- [ ] Stage FK assignments and reject cardinality conflicts before modifying rows. Apply updates only to rows inserted by this import under the initial policy. Preflight nonnullable/cyclic FK insertion order: support a feasible constraint-respecting order or report unsupported cases before writes; do not disable constraints or assume all constraints are deferrable.
- [ ] Verify reverse traversal after both relationship imports and retain placement/partition-cut/proxy metadata outside application attributes. Validate remote references according to the explicit policy, without inventing rows.
- [ ] Force mid-import failures and assert rollback of node/edge/FK/metadata changes. Run focused tests; commit each relationship mode separately.

## Task 8 — #22 and #12: Integration, documentation and acceptance

**Files:** Create `tests/fixtures/relational_postgresql/` with schema, seed data, mappings and hand-checked BFS results; `tools/ingestion/relational_live.py`; `docs/examples/relational-postgresql/README.md`. Modify `tests/CMakeLists.txt`, documentation entry points and PostgreSQL storage docs.

- [ ] Reuse disposable live-test lifecycle for isolated PostgreSQL plus existing Neo4j/ArangoDB services. Register `integration.postgres-relational`; an explicitly requested run must fail if services are unavailable rather than report acceptance from skipped tests.
- [ ] Exercise two workflows: attach an already populated native database read-only, and import into pre-created empty typed tables. Include unrelated populated sentinel tables and compare schema/data before and after.
- [ ] Cover integer/text/UUID keys, overlapping IDs, multiple SQL schemas, nullable/invalid FKs, both edge representations, properties, parallel edges, cycles, self-loops, unknown mappings and generation mismatch.
- [ ] Compare hand-checked BFS distances and valid predecessors across PostgreSQL-only and PostgreSQL → Neo4j → ArangoDB → PostgreSQL paths, with forward/reverse/both directions, type filters, partition cuts and proxies. Require rich/minimal/streamed parity.
- [ ] Document mapping format, key/type limits, attachment and generation refresh, import append policy, FK cardinality/cross-backend limits, required permissions, generic-mode compatibility and recoverable import publication failures.
- [ ] Run full CPU regressions, live relational/mixed-backend suite, generic live smoke, documentation/link checks and whitespace checks. Record actual commands/results and any unavailable checks before claiming completion.
- [ ] Review every acceptance criterion in #13–#22 against tests and documented limitations. Keep #12 open until unsupported cases have been explicitly accepted or implemented. No automatic issue closure, merge or deployment.

## Verification commands

Existing baseline commands (run at implementation time, not executed for this planning-only change):

```sh
cmake --preset cpu-tests
cmake --build --preset cpu-tests -j 2
ctest --preset cpu-tests
python3 tools/docs/check_links.py
git diff --check
```

New test registration must make these commands meaningful:

```sh
ctest --test-dir build/cpu-tests -R 'Relational' --output-on-failure
ctest --test-dir build/cpu-tests -R '^integration.postgres-relational$' --output-on-failure
```

Keep live integration opt-in consistent with current CMake flags. Do not count unregistered tests, skips or a unit-only run as proof of PostgreSQL or cross-backend acceptance.

## Review decisions before implementation

1. Accept scalar integer/text/UUID keys initially, or include composite keys in #12? Composite keys require extending the current scalar source-identity contract deliberately.
2. Accept immutable attachment generations and explicit refresh, or require live synchronization with application writes? CDC would be substantial additional scope.
3. Accept append into empty mapped destinations first, or require safe updates/merges into populated application tables? Existing-row writes need conflict and recovery semantics beyond read-only attachment.
4. Accept separate metadata plus temporary runtime joins, and explicit rejection of relational constraints that cannot represent remote-only endpoints, or define an application-specific reference-row policy?

These are proposals to review, not blockers to producing this plan. The next implementation step is #13 once the contract decisions are settled.

## Issue #13 execution record — 2026-11-09

Task 1 implemented with public immutable mapping/metadata APIs, backend-relative
configuration references, a matching person/post/comment example, actionable
validation failures, and live PostgreSQL tests. Metadata inspection uses only
parameterized SELECTs and preserves application tables.

Scope decisions: validation is read-only in #13; write privilege/default/required
column feasibility belongs to #18. The runtime parses mapped configuration but
explicitly refuses relational traversal until #21. The native importer rejects
`relationalMapping` before selecting a generic executor until #18. Import mapping
artifact binding is deferred to that pipeline. No IDs are allocated in #13.

Independent review found operator/type shadowing through search_path and ordinary
inheritance PK ambiguity. Both were reproduced in failing live tests and fixed:
all catalog operators/casts are qualified, and ordinary inheritance parents are
rejected while declarative partitioned tables remain accepted. No deferred review
findings remain.

Verification:

- `cmake --build --preset cpu-tests -j 2`: passed.
- Full `ctest --test-dir build/cpu-tests --output-on-failure -j 2` with the
  disposable PostgreSQL DSN and local socket access: 612 passed, 3 skipped,
  zero failures (615 registered tests). The live PostgreSQL entry ran all 15
  metadata cases; skipped tests were the two optional KaMinPar paths and the
  separate opt-in unfamiliar-schema backend live test.
- An initial sandbox-restricted run failed the existing Redis loopback test with
  `socket failed`; it passed in the complete socket-enabled run. No Redis change.
- Mapping/config/import-guard focused selection: 14 passed.
- `clang-format-21 --dry-run --Werror` on changed C++, documentation link checker,
  and `git diff --check`: passed. System clang-format 14 cannot read this repo's
  format configuration; version 21 was used.

No commits, issue closure, or later implementation tasks were performed.
