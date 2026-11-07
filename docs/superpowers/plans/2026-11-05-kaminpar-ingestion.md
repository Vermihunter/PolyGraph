# KaMinPar Ingestion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add real optional topology partitioning before native database ingestion, with compatible IDs/placement and measured overhead.

**Architecture:** Optional shared-memory KaMinPar library behind the existing CMake flag; existing input adapters and SQLite own source-key mappings and projection edges. Reuse normalization and native importers, adding versioned partition metadata and strict/live verification.

**Tech Stack:** C++23, GoogleTest/CTest, SQLite, KaMinPar v3.7.4/oneTBB, Python standard-library tooling, existing isolated Docker harness.

**Spec:** `docs/superpowers/specs/2026-11-05-kaminpar-ingestion-design.md`

## Global Constraints

- Keep the demo, input datasets, existing containers/databases and runtime/BFS code unchanged.
- Ordinary CPU builds/tests must not fetch or require KaMinPar; unavailable requests fail explicitly.
- Nodes/proxies: `partition_id`; relationships/replicas: `from_partition_id`, `to_partition_id`, `is_partition_cut`.
- Preserve canonical IDs; explicit raw mapping happens before final IDs. Relationship ownership stays schema-based.
- Default partition threads 1, imbalance 0.03, preset `default`, explicit count >=2, existing seed 42.
- No commits/pushes requested. Work in the user's existing clean feature branch. Keep generated data under ignored build output.

## Review Focus

- Canonical/signed/residency-decorated IDs must never become CSR indices or partition numbers; exercise canonical remap rejection and stable IDs.
- Directed duplicates, self loops, isolates and missing endpoints must project correctly without losing original import records.
- Sparse external partition numbers, malformed negative/fractional values, missing/extra assignments and unmapped partitions must fail clearly.
- Older manifests/checkpoints must remain verifiable; changed partition settings/dependency must invalidate stale preparation.
- Metadata collisions, corrupt assignments and missing/wrong live partition fields must fail verification rather than overwrite or pass silently.

## Task 1: Optional real partitioner and topology API

**Files:** `include/ingestion/Partition.hpp`, `src/ingestion/Partition.cpp`, `src/ingestion/Kaminpar.cpp`, `src/ingestion/CMakeLists.txt`, `cmake/Options.cmake`, `cmake/KaMinPar.cmake`, `tests/ingestion_partition_test.cpp`, `tests/CMakeLists.txt`.

**Interfaces:** `GraphTopology` (uint64 offsets/uint32 neighbors), `PartitionOptions` (count/threads/imbalance/preset), `PartitionResult` (assignments/populations/cut/balance), availability/dependency identity and `partition_topology(graph, options, seed)`.

- [x] Write graph/options/unavailable tests, observe RED, implement validation and the OFF path, run focused CPU tests GREEN.
- [x] Integrate pinned optional library without changing default dependencies; configure an enabled build using a read-only cached upstream source when available.
- [x] Test actual two-clique partitions, isolated nodes, sequential repeatability, count/balance/cut and invalid results/graphs; build and run enabled tests.

## Task 2: Disk-backed projection, placement and artifacts

**Files:** new `src/ingestion/PartitionStage.cpp`; existing `Import.hpp`, `Internal.hpp`, `Import.cpp`, `Normalize.cpp`, `Writers.cpp`; focused ingestion tests.

**Interfaces:** prepare source-key assignments before final node ownership/IDs; pass an additive metadata-version flag to backend generation/strict reconstruction.

- [x] Write failing external-assignment/metadata/ID/old-manifest tests; implement strictly validated mapping and the disk-backed projection using existing adapters.
- [x] Bound CSR buffers, call real KaMinPar, persist/export assignments atomically, record projection/compute/assignment/cut/population metrics.
- [x] Add node/proxy/edge metadata to all native representations without changing PostgreSQL table/COPY schema; reject reserved input collisions.
- [x] Verify assignment checksum/closure/fields and old artifact compatibility; exercise malformed options/maps, duplicates/loops/isolates, missing endpoints and stale/corrupt checkpoints.

## Task 3: CLI, Docker workflow and live correctness

**Files:** `app/bulk_import/main.cpp`, `tools/ingestion/docker_load.py`, `src/ingestion/Execute.cpp`, `Postgres.cpp`, `Arango.cpp`, `Neo4j.cpp`, public workflow tests/live harness.

- [x] Add failing CLI/forwarding tests; expose/validate partition controls through prepare and load, preserving explicit fresh-target safety.
- [x] Extend bounded expected-row/live queries to validate generated partition fields and detect wrong metadata.
- [x] Run partitioned disposable raw and canonical imports, map/no-map cases, exact native metadata checks, actual cross-backend BFS, safe cleanup and unchanged existing services.

## Task 4: Benchmarks, docs and final checks

**Files:** preparation-overhead benchmark tooling/tests, examples, ingestion/build guides and benchmark records/index.

- [x] Benchmark none versus real partitioning with matched binary/input/config on SF0.1; retain provenance, CPU/RSS/disk, cuts/balance and timings. Verify artifacts and structural ID/endpoint/placement identities outside timing.
- [x] Document runnable build/load commands, partition metadata, projection/weights/balance/determinism/memory, mapping precedence, recovery and actual measurements/limits.
- [x] Run focused/full CPU and Python regressions, enabled partition tests, formatting, targeted sanitizers and a fresh read-only final review. Record exact results and final diff.

## Execution ledger

- Base: `71e7510`, clean `feature/native-bulk-ingestion`; user requested direct implementation and supplied the demo reference. Existing feature checkout is used; no new worktree or Git mutation is needed.
- Ruling: use existing PostgreSQL JSONB for partition properties instead of altering runtime tables; this preserves legacy COPY/readers and avoids a cross-cutting schema migration.
- Ruling: match the demo's simple undirected/unit-weight objective while preserving all original directed/multiple/self-loop import records; ghost suggestions do not replace backend proxy policy.

- Task 1: OFF tests 4/4 and real-library tests 5/5 passed. Enabled build uses read-only cached demo source at the pinned 04dfa70 commit. Upstream reports header version 3.7.3 despite the v3.7.4 tag.
- Debug finding: copying the demo's KAssert-OFF setting is invalid for Debug: KASSERT_ENABLED=0 skips sentinel initialization while assert() still checks it. GDB showed an initial partition slot 0 instead of UINT32_MAX, on the first assignment. Enable the pinned KAssert dependency; normal assertion-level enabled tests pass. No dependency assertions were disabled to pass tests.
- Task 2 RED: three new external-metadata/malformed/collision tests failed against old behavior, then all 35 ingestion tests passed after integration. Generated pipeline test caught an unstable SQLite cursor while updating the same scanned table; construct dense IDs in a separate ordered INSERT SELECT instead.

- Task 2 generated pipeline tests now pass 7/7 with real KaMinPar: canonical signed IDs/residency, properties, endpoints, proxies/replicas unchanged; duplicates/reverse arcs/self loops affect only the objective. Canonical remapping is rejected. All 35 ordinary ingestion tests passed. Python full-representation oracle now passes 4/4 for both raw/canonical adapters with partition fields; checksum-consistent assignment tampering is rejected by strict verification.
- Task 3 CLI RED→GREEN forwarding and pre-provision validation tests passed. Added typed metadata checks to all three native live verifiers without modifying SQL table/COPY schemas or BFS/runtime.

- Task 3 final actual live run: `python3 tools/ingestion/docker_load_live.py --run --binary build/cpu-partition-tests/bin/polygraph_bulk_import --bfs build/cpu-tests/bin/polygraph_bfs --partitioner kaminpar --cases keep canonical mapped --output-dir build/ingestion-partition/reviewed-live` passed. Report: `reviewed-live/run-45e009acfc51/result.json`. All three cases verified/BFS; six native metadata corruption checks detected/restored; 48 pre-existing IDs and 33 unrelated running states preserved. Earlier overlapping smoke cleanup caused one preservation assertion failure; it was retained, then sequential tests passed. No validation was weakened.
- Final fresh read-only review identified two P2 recovery findings. Both fixed RED→GREEN: `PartitionCheckpointRejectsMissingOrCorruptAssignmentsBeforePublication` (three missing/corrupt/prepublication variants) and `DependencyIdentityIncludesActualLibraryModuleContents`. Runtime module SHA-256 covers installed same-version replacements, dirty source and TBB; static linkage conservatively hashes the executable. No deferred substantive findings. No second review.
- Final suite evidence before benchmark: `ctest --preset cpu-tests --output-on-failure` passed 519/519; enabled ingestion selection passed 50/50 including 11 real partition tests and 36 ordinary ingestion tests; Python ingestion modules passed 38/38, graphio discovery 31/31, explicit loader/LDBC modules 16/16 (12 LDBC + 4 loaders). Old native binary-generated none/external manifests passed new strict verification.
- Focused partition API compiled with clang++-21 ASan/UBSan and leak detection: 4 cases ×100 repetitions passed. Sandbox LeakSanitizer inspection initially failed because of ptrace; the identical command passed outside the sandbox with leak detection enabled. This does not instrument the upstream KaMinPar library.
- Task 4 benchmark helper smoke completed all six tiny trials, strict verification and graph fingerprints. SF0.1 six-trial benchmark started at `build/ingestion-partition/sf01-overhead-001`; no builds/tests/Docker jobs overlap measurement. Engine and collector sources are frozen until completion. Database bind data from already-removed task-owned initial disposable tests was removed after exact container-ID absence checks to free disk, retaining logs/artifacts; existing environments were untouched.

- External filesystem interference at 16:50: completed trial directories in `sf01-overhead-001` disappeared during the final prepare, which failed. Logs retained but five detailed measurement files were lost; do not publish reconstructed/partial numbers as a successful benchmark. Input/native binaries/source remained present. Ruling: rerun a fresh single repetition per variant under `/tmp/polygraph-kaminpar-sf01-20261105-rerun-001` on the same ext4 NVMe, retaining complete bundles; publish as n=1 with no variability/statistical or SF0.1-repeatability claim. Small-graph fixed-seed repeatability tests remain valid. This yields measured overhead without another full repeated campaign after an external interruption; the cost is no estimate of variance. User asked asynchronously about active output cleanup.

- Task 4 completed rerun: `/tmp/polygraph-kaminpar-sf01-20261105-rerun-001/benchmark.json`, copied complete bundles to `build/ingestion-partition/sf01-overhead-complete-001`. Three strict-verified trials have the same original graph fingerprint. Native prepare wall/RSS: none 84.61s/228.625MiB; k2 143.50s/273.8125MiB; k8 146.97s/283.66015625MiB. Library compute 4.55283522s and 6.815042301s. n=1, no variance/SF repeatability claims; failed prior campaign excluded. Published sanitized complete measurements and phase/CPU/disk/writer/cut/balance tables. Native binary hash b6bb9e3b477e6bfc757eae48a79ef4c4c9623282dcccb38d442323e0576ab0f4. Engine and collector sources stayed frozen during measurement.
- Final formatting: clang-format-21 --dry-run --Werror passed all 17 changed/new C++ files. `git diff --check` passed. No deferred review findings; installed/shared dependency deployments, larger scales, weighted objectives and native-load/traversal speed benefits remain untested or unsupported as documented.

- Final guide build succeeded: `cmake --build --preset cpu-partition-tests --target polygraph_bulk_import polygraph_bfs polygraph_ingestion_partition_test -j2`. Optional `integration.ingestion.kaminpar` registration verified with live opt-in ON and `ctest -N`, then option restored OFF. Documentation links and complete copied benchmark evidence checked, source/native/collector hashes match the measurements. Final diff summary before this ledger line: 39 files changed, 2954 insertions(+), 125 deletions(-) (including new files) . No Git commit/push requested or performed.
