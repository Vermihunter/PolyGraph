# Native ingestion validation and measurements — 2026-11-03–04

This report describes the implementation and the checks actually run in the development environment. The runnable guide is [Native bulk ingestion](../guides/BULK_INGESTION.md). Generated datasets, logs, manifests and measurement JSON remain under ignored `build/` paths.

## Implementation

- `include/ingestion/`, `src/ingestion/`: streaming JSONL/CsvBasic adapters, explicit placement, original GlobalId codec, LDBC block31 IDs, disk-backed normalization/endpoint/proxy index, three native writers, checksummed manifests, checkpoints, safe argv execution, native import executors and exact ID/endpoint verification.
- `app/bulk_import/`: independent `polygraph_bulk_import` CLI. CMake options and `cpu-tests` integration do not change the runtime/query path.
- `tests/ingestion_test.cpp`, `tests/fixtures/ingestion/`, `tools/ingestion/test_semantics.py`: CPU regressions, golden native representations and Python compatibility tests.
- `tools/ingestion/disposable_live.py`: opt-in actual native tools, stored property checks, three-backend BFS and legacy loader comparison on isolated targets.
- `tools/ingestion/benchmark.py`: reproducible preparation and optional import-command measurements.
- `docs/guides/BULK_INGESTION.md`, `docs/examples/ingestion/`: commands, target/placement examples, memory/safety/recovery limits and extension points. README and documentation index link the guide.

The implementation uses a durable disk spool instead of retaining vertices/edges or an unbounded queue in RAM. Independent backend workers own their output buffers and read connections. PostgreSQL proxies retain logical types, Neo4j uses stable `localId` and signed proxy/replica `uuid`, and Arango uses existing collection/key/handle conventions. Input mutation invalidates checkpoints permanently; uncertain remote execution is never retried automatically. Fresh-target preflight precedes mutation. PostgreSQL locks/rechecks its tables, Arango atomically reserves its fresh database, and Neo4j requires an operator-provided offline storage check and refuses overwrite.

A fresh code review identified checkpoint poisoning after input mutation, sparse Neo4j string/null handling, schema-union growth, duplicate CSV property names and Arango preflight/reservation gaps. Each was corrected; targeted regressions and the final CPU/live checks cover those changes. Unicode sanitization, source-ID prefix/sequence parity, signed-ID boundaries and Arango collection namespace collisions also have explicit tests.

## Commands exercised

```bash
cmake --preset cpu-tests
cmake --build --preset cpu-tests
build/cpu-tests/bin/polygraph_ingestion_test
POLYGRAPH_BULK_IMPORT="$PWD/build/cpu-tests/bin/polygraph_bulk_import" \
  python3 -m unittest tools.ingestion.test_semantics -v
ctest --preset cpu-tests --output-on-failure

python3 -m unittest discover -s tools/graphio -t . -p 'test_*.py'
python3 -m unittest discover -s tests -t . -p 'test_*.py'
python3 -m unittest discover -s tools/ldbc_snb -t . -p 'test_*.py'

cmake --preset cpu-tests -DPOLYGRAPH_INGESTION_LIVE_TESTS=ON
cmake --build --preset cpu-tests --target polygraph_bulk_import polygraph_ingestion_test polygraph_bfs
ctest --test-dir build/cpu-tests -L live --output-on-failure
cmake --preset cpu-tests -DPOLYGRAPH_INGESTION_LIVE_TESTS=OFF

python3 tools/ingestion/disposable_live.py --run --adapter polygraph-jsonl \
  --workflow native --binary build/cpu-tests/bin/polygraph_bulk_import \
  --bfs build/cpu-tests/bin/polygraph_bfs --output-dir build/ingestion-live/final-001

python3 tools/ingestion/disposable_live.py --run --adapter ldbc-snb \
  --workflow native --binary build/cpu-tests/bin/polygraph_bulk_import \
  --bfs build/cpu-tests/bin/polygraph_bfs --output-dir build/ingestion-live/native-compare-001
python3 tools/ingestion/disposable_live.py --run --adapter ldbc-snb \
  --workflow python --binary build/cpu-tests/bin/polygraph_bulk_import \
  --bfs build/cpu-tests/bin/polygraph_bfs --output-dir build/ingestion-live/python-001

clang-format-21 --dry-run --Werror include/ingestion/*.hpp src/ingestion/*.cpp \
  src/ingestion/*.hpp app/bulk_import/main.cpp tests/ingestion_test.cpp
git diff --check
```

Final results: **494/494 ordinary CTest cases passed**, including **25 ingestion GoogleTests** and the **three Python native-representation oracle cases**. Separate Python regression commands passed 31 graphio, 88 repository/reference, and three LDBC-tool tests. Both opt-in live CTest cases passed; the final canonical run passed exact stored properties and BFS from six roots.

The pre-existing Redis fake-server test requires local socket creation; running the ordinary suite inside a socket-denying sandbox caused `socket failed`. The full suite was rerun with local socket access, without external database dependencies. No test was skipped or weakened to address that environmental failure. The environment's GTest RPATH selects a Conda libcurl and prints a version-information warning; tests and the standalone CLI still run. Production/live CLI requests were validated with the actual service containers.

The separate Release configuration also built and prepared/strict-verified the fixture with runtime, GPU, backend adapters, tests, and native execution dependencies disabled:

```bash
cmake -S . -B build/ingestion-only -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPOLYGRAPH_ENABLE_GPU=OFF -DPOLYGRAPH_BUILD_BACKENDS=OFF \
  -DPOLYGRAPH_BUILD_BFS=OFF -DPOLYGRAPH_BUILD_HTTP=OFF \
  -DPOLYGRAPH_BUILD_TESTS=OFF -DPOLYGRAPH_BUILD_BULK_IMPORT=ON \
  -DPOLYGRAPH_INGESTION_NATIVE_EXECUTORS=OFF \
  -DPOLYGRAPH_ENABLE_CLANG_FORMAT=OFF -DPOLYGRAPH_ENABLE_CLANG_TIDY=OFF \
  -DFETCHCONTENT_SOURCE_DIR_SPDLOG="$PWD/build/cpu-core/_deps/spdlog-src"
cmake --build build/ingestion-only --target polygraph_bulk_import -j2
build/ingestion-only/bin/polygraph_bulk_import prepare --adapter ldbc-snb \
  --input tests/fixtures/ingestion/raw --placement-config tests/fixtures/ingestion/placement.json \
  --output-dir build/ingestion-standalone-check
build/ingestion-only/bin/polygraph_bulk_import verify \
  --manifest build/ingestion-standalone-check/manifest.json --strict
```

The local cached spdlog source override avoided a network fetch in this restricted environment; spdlog is part of the existing top-level CMake setup and is not a linked ingestion dependency.

## Preparation measurement

Machine: AMD Ryzen 7 5800H, 16 logical CPUs; GCC 11.4, C++23 configuration; Linux. The generated workload has **30,000 nodes and 30,000 relationships**, three node labels and three relationship types, with each authoritative backend represented. There is no partitioning. Three sequential warm-cache repetitions used three writer threads and a 256 MiB cache/record budget. Native preparation writes 150,000 physical node/proxy/edge records after replication, the disk index, native artifacts and hashes. Legacy Python conversion writes canonical JSONL, schema and reports. **These stages perform different work; the table is not an equal-work speedup claim.**

```bash
python3 tools/ingestion/benchmark.py --binary build/cpu-tests/bin/polygraph_bulk_import \
  --output-dir build/ingestion-benchmark/debug-002 --nodes-per-backend 10000 --repetitions 3
python3 tools/ingestion/benchmark.py --binary build/ingestion-only/bin/polygraph_bulk_import \
  --output-dir build/ingestion-benchmark/release-001 --nodes-per-backend 10000 --repetitions 3
```

| Preparation workflow | Mean elapsed | Sample standard deviation | Maximum client RSS |
|---|---:|---:|---:|
| Native, Debug | 3.167 s | 0.131 s | 46,840 KiB |
| Legacy conversion in the Debug comparison | 1.247 s | 0.050 s | 27,688 KiB |
| Native, Release | 1.113 s | 0.061 s | 40,968 KiB |
| Legacy conversion in the Release comparison | 1.300 s | 0.020 s | 27,492 KiB |

The first Release trial measured about 72,672 input nodes/s and 80,726 input edges/s during their respective normalization phases; preparation elapsed was 1.078 s. Its writer-stage figures include serialization, buffered writes, fsync and checksums:

| Backend | Native nodes | Proxies | Relationships | Artifact bytes | Writer bytes/s |
|---|---:|---:|---:|---:|---:|
| Neo4j | 10,000 | 10,000 | 20,000 | 4,857,095 | 25.80 MB/s |
| ArangoDB | 10,000 | 20,000 | 30,000 | 9,617,780 | 36.37 MB/s |
| PostgreSQL | 10,000 | 20,000 | 20,000 | 6,047,780 | 39.59 MB/s |

Inputs, commands, per-trial statistics, partitioning availability and backend statistics are recorded in each `benchmark.json`. Peak RSS is measured with `/usr/bin/time`; the memory option is a cache/record budget, not a hard RSS limit. This small synthetic benchmark is not a production-scale performance validation.

## Live validation and import comparison

Cached images: Neo4j 5.26 Enterprise, ArangoDB 3.12 and PostgreSQL 17. Tests created fresh per-run containers, unique loopback ports and bind-mounted Neo4j storage under the ignored output directory. They did not reuse/reset existing PolyGraph services or volumes. Test-owned containers were stopped/removed after their results and logs were captured.

The raw fixture has three logical nodes and three relationships whose owners cover all three backends. After placement/replication, Neo4j holds 1 native node, 1 proxy and 2 relationships; Arango holds 1 native node, 2 proxies and 3 relationships; PostgreSQL holds 1 native node, 2 proxies and 2 relationships. Arango's reservation marker is excluded from graph counts. The canonical variant adds three isolated Neo4j nodes and exercises empty arrays, delimiter/empty string array elements, booleans, floats, sparse strings, explicit null and empty strings. It verifies BFS from all six roots.

Both adapters passed the opt-in CTest integrations. The final canonical run additionally exercises batched property repair, relationship identity indexes, exact live IDs and endpoint traversal, stored properties and the existing distributed BFS runtime. Reports are `integration-result.json`, `live-verification.json`, `execution-state.json` and the BFS CSV files in the corresponding build directory.

A **single, tiny correctness-scale** comparison on identical raw input, images, DB settings and per-run container cleanup measured:

| Workflow | Import plus startup elapsed |
|---|---:|
| Native workflow | 24.525 s |
| Existing Python insertion paths | 18.103 s |

Settings: Neo4j heap 512 MiB / page cache 128 MiB; Arango block cache 64 MiB / cache 32 MiB; PostgreSQL image defaults; optional PostgreSQL traversal indexes off. The Python comparison uses the existing fresh insertion paths, replacing destructive Neo4j reset with a read-only assertion that this newly created store is empty. Native time includes offline importer startup, subsequent DBMS startup and lifecycle verification; Python time includes online DBMS startup and loading. Startup dominates; there is no steady-state import throughput or performance improvement claim. Separate final correctness runs are not substituted into this recorded comparison. Native per-backend executor times/throughput are in the comparison execution journal.

## Unsupported or unverified

- KaMinPar is **not implemented**; CMake/runtime requests fail explicitly. External partition assignments and explicit partition-to-backend mapping work. No KaMinPar edge-cut, balance, weight or memory/determinism claim is made.
- No Parquet adapter, incremental/online Neo4j mode, automatic database provisioning, uncertain remote-stage retry, parallel Arango HTTP batching or large-dataset import performance claim.
- Neo4j cannot natively represent nested objects or heterogeneous/nested/null-containing arrays; preparation rejects those when destined for Neo4j. Explicit scalar/date/time strings and supported arrays are preserved.
- TSan compilation reached GoogleTest discovery, but the environment's sanitizer runtime aborts before test execution with `FATAL: ThreadSanitizer: unexpected memory mapping`. This is **not** a passing sanitizer run. No full clang-tidy sweep or large-scale memory-pressure test was run.
- Docker/Kubernetes runners outside the provided disposable harness and backend versions other than those listed were not live-tested. Operator-provided Neo4j offline preflight is an explicit trust boundary.
