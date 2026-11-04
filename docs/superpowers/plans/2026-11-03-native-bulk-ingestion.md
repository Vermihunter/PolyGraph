# Native bulk ingestion implementation plan

**Goal:** Implement the user-specified standalone, CPU-only native ingestion pipeline.
**Architecture:** Streaming adapters feed a disk-backed SQLite normalization and endpoint index. Backend workers independently read resolved records, emit atomic native artifacts, and publish a SHA-256 manifest. Execution is separate, requires explicit fresh named targets, and fails closed on ambiguous recovery.
**Tech stack:** C++23, existing vendored nlohmann JSON and GlobalId codec; SQLite3, OpenSSL SHA-256; optional libpq/libcurl execution.
**Spec:** User task “Implement PolyGraph Native Bulk Ingestion Engine” in this session.

The user explicitly requested direct implementation without a design approval stop. Work stays in the provided feature checkout, with no commits or destructive live operations.

## Constraints and decisions
- Preserve Python ldbc-block31-v3 IDs, file/type iteration order, edge replication, proxy and native ID representations.
- Preserve canonical JSONL IDs, validate placement and residency; reject unsupported types/configuration instead of falling back.
- SQLite cache and individual record sizes are bounded; the graph and auxiliary properties remain on disk.
- KaMinPar is unsupported until an actual integration is present; both configuration and runtime requests fail explicitly. Accept externally supplied partitions with explicit backend mapping as an independent mode.
- CPU tests do not require services; live tests are opt-in and require isolated targets. No claim of live correctness without running them.

## Tasks
1. [x] Tests first: ID, placement, parsing, three-backend fixture and Python representation oracle. Build focused ingestion tests, implement adapters/index/placement.
2. [x] Artifact writers, atomic publication, checksums and strict semantic verification. Test escaping, signed IDs, proxy replication, corruption, failure recovery, stale input.
3. [x] Safe process runner and native import executors; execution journal, fresh-target preflight, concurrent backend limits and live verification. Test failed/missing commands and concurrency without services.
4. [x] CLI/CMake, examples, opt-in integration tooling and benchmark runner. Run focused tests, CPU suite and Python regressions; measure preparation on synthetic input only.
5. [x] Review and final verification. Format changed C++; report exact commands, outcomes and limitations, and git diff summary.

## Review focus
- Canonical residency variants aliasing the same physical ID.
- Missing endpoints and duplicate IDs must prevent manifest publication.
- Importer failure after partial remote writes must never be auto-retried.
- Concurrent prepare/execute invocations and path traversal through a manifest.
- Inconsistent property types and Neo4j CSV array delimiters must not corrupt values silently.

## Verification ledger

- Implemented in the provided feature checkout; no commits or database resets were performed.
- Focused tests: 25 ingestion GoogleTests and three Python representation/ID oracle cases passed.
- Full ordinary suite: 494/494 CTest cases passed with local socket access for the existing Redis fake.
- Python regressions: graphio 31, repository/reference 88, LDBC tools three passed.
- Both opt-in live CTest cases passed. Final canonical native run validated exact properties, indexed ID/endpoint checks, batched repairs and BFS from six roots.
- Native and existing Python insertion workflows were measured on the same disposable raw fixture/settings, with no speedup claim. Preparation measured separately in Debug and Release.
- Fresh review findings fixed: invalidated input-mutation checkpoints, sparse Neo4j null/string repairs, schema bounds, duplicate CSV headers, Arango fresh-target reservation. Added Unicode/namespace boundary regressions.
- TSan runtime blocked at discovery by unexpected memory mapping; this is not a passing sanitizer result.
- KaMinPar is explicitly unsupported, as allowed by the requested initial-delivery policy. No fake fallback.
- Detailed evidence and limitations: docs/benchmarks/NATIVE_BULK_INGESTION.md.
