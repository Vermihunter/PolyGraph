# One-command Docker ingestion

From the repository checkout, build the CPU executables, then import the dataset and verify it separately:

```bash
cmake --preset cpu-tests
cmake --build --preset cpu-tests --target polygraph_bulk_import polygraph_bfs

build/cpu-tests/bin/polygraph_bulk_import load \
  --adapter ldbc-snb \
  --input data/ldbc-snb/sf0_1/raw \
  --placement-config tools/ldbc_snb/placement.schema-3db.json \
  --docker \
  --keep-running \
  --skip-verification \
  --output-dir build/ingestion-load/sf01-fast

build/cpu-tests/bin/polygraph_bulk_import verify-load \
  --output-dir build/ingestion-load/sf01-fast \
  --verify-bfs
```

The first command performs preparation, fresh Docker provisioning and all native imports, then leaves the services running. Its result is **`state: imported`, `verification: skipped`**. The second command performs strict artifact reconstruction and exact live correctness checks, plus depth-two cross-backend BFS when requested. It obtains targets and credentials from the saved load directory; no environment exports or target JSON editing are needed.

An explicit output directory must be new or empty. Choose a different directory for another import. Keep `--keep-running` when you intend to verify afterward. Verification requires the owned services to be running; it never starts, stops, removes, repairs or reimports them.

`--skip-verification` skips exhaustive artifact reconstruction, live ID/type/count/endpoint scans, and PostgreSQL's in-transaction correctness scan. Input parsing, collision/endpoint resolution during preparation, artifact checksums, fresh-target preflight, native tool error handling, COPY/import acknowledgements and journal safeguards remain mandatory. Native constraints, indexes and representational repairs still run because they are part of constructing the graph.

Omitting `--skip-verification` keeps verification in the load command. Full native execution publishes its verification report, which the Docker wrapper reuses rather than running a duplicate live pass. `--skip-verification` cannot be combined with `--verify-bfs`; request BFS in the separate verification command instead. Manual `execute` supports the same `--skip-verification` option.

No Compose file, database creation, target JSON, password export or offline runner configuration is needed. The standard `static/`, `dynamic/`, `params.ini` and root update-stream layout is accepted. Initial snapshot ingestion ignores update events. Existing snapshot validation and canonical IDs are unchanged.

Requirements are the built native executors, Python 3.10+ (standard library only for this workflow), this source checkout, and access to a local Docker daemon. Existing `prepare`, `execute` and `verify-live` commands remain available and keep their existing behavior. `load` is a lifecycle wrapper around those commands, not a separate importer.

## What the command does

1. Creates a new private output directory and fresh generation name.
2. Runs C++ `prepare`, plus `verify --strict` unless verification is deferred, before provisioning any services.
3. Selects cached repository images when available, otherwise pulls official `neo4j:5.26-enterprise`, `arangodb:3.12`, and `postgres:17`. Overrides are available. Resolved image IDs are recorded and used for container creation.
4. Creates a uniquely named network and three new containers, with new bind-mounted storage under that output directory. Published ports bind to `127.0.0.1`.
5. Generates random credentials, `targets.json`, and private lifecycle state. PostgreSQL creates the named database during initialization; the controller creates the named ArangoDB database.
6. Keeps Neo4j stopped while the existing executor runs `neo4j-admin database import full` against its fresh storage. The shared runner starts the owned DBMS afterward, checks the `system` catalog, registers the generated database if necessary, and waits for it to become queryable. It never overwrites an existing store.
7. Runs the existing native executors for all configured backends. Full mode performs strict artifact and exact live checks during execution; deferred mode records completed imports without those exhaustive checks.
8. Prints a JSON result with generation/container names, HTTP/SQL/Bolt endpoints, database names, credentials location, logical and physical counts, elapsed time, and an ownership-checked stop/remove command.

`--keep-running` leaves imported services running. Without it, the controller stops/removes only its owned containers and network after the selected workflow. Artifact files, database bind directories, logs and private credentials remain in the output directory in both cases.

Neo4j license acceptance follows the repository's existing Enterprise integration setup. Use an image you are authorized to run. No production service is stopped or reused.

## Output and connection information

The default output is `build/ingestion-load/<fresh-generation>/`. Every generation has a random suffix even when a readable prefix is supplied:

```bash
build/cpu-tests/bin/polygraph_bulk_import load \
  --adapter ldbc-snb --input data/ldbc-snb/sf0_1/raw \
  --placement-config tools/ldbc_snb/placement.schema-3db.json \
  --docker --keep-running \
  --output-dir build/ingestion-load/my-sf01-run \
  --generation-name sf01
```

An explicit output directory must be absent or empty. It cannot contain commas/newlines because it is used in Docker bind-mount syntax. Existing or failed output directories are rejected; generation names never authorize reuse. `--generation-name` is a lowercase letter/digit/dash prefix; the complete generated name is limited to 56 characters.

| File/directory | Purpose |
|---|---|
| `load-result.json` | Import result, verification status, endpoints, manifest counts, duration and cleanup command |
| `verification-result.json` | Latest separate verification result/error, exact native report, optional BFS and verification duration |
| `credentials.json` | Generated usernames/passwords and connection information; mode `0600` |
| `targets.json` | Existing native executor configuration; secrets referenced through the controller's child environment |
| `environment.json` | Exact Docker IDs, ownership generation, image IDs, mounts, ports and lifecycle status |
| `runtime.secret.json` | Generated connection configuration for the existing PolyGraph BFS executable; mode `0600` |
| `runtime-schema.json`, `runtime-data.jsonl` | Typed CPU BFS schema and routing metadata when representable |
| `artifacts/` | Native manifest, disk index, artifacts and execution/live-verification journals |
| `logs/` | Per-stage stdout/stderr, native tool diagnostics and server logs |
| `docker/` | Fresh per-backend bind-mounted storage; backend UID permissions are retained |

The output directory is private (`0700`); secrets and diagnostic files are private. Passwords are never placed in command arguments or printed in the result. Read `credentials.json` locally to connect. Do not commit generated output or private runtime configuration. Existing `.gitignore` rules cover the default `build/` location.

`load-result.json.duration_seconds` records the initial load's elapsed time. A later `verify-load` records its own elapsed time in `verification-result.json.duration_seconds` and `load-result.json.verification_duration_seconds`, preserving the original load duration. Before correctness verification, `counts_source: manifest` identifies counts derived from prepared artifacts. Successful verification changes the load result to `state: verified`, `verification: full`, `counts_source: live-verification`. The import journal remains an unchanged historical record of the import-only operation. A verification failure records `verification: failed`, returns nonzero and leaves all services and import journals untouched; stage logs retain every attempt.

`runtime-data.jsonl` contains routing metadata for CPU BFS, not a replacement graph dump. A native-valid JSONL graph can use a relationship label with several endpoint type pairs; the current typed BFS schema cannot represent that shape. Such an import still succeeds without BFS validation, and `runtime-bootstrap.json` records why no schema was produced. Requesting BFS for an unsupported schema fails before Docker provisioning.

## BFS validation

Add `--verify-bfs` to run the actual distributed CPU BFS after ingestion:

```bash
build/cpu-tests/bin/polygraph_bulk_import load \
  --adapter ldbc-snb --input tests/fixtures/ingestion/raw \
  --placement-config tests/fixtures/ingestion/placement.json \
  --docker --keep-running --verify-bfs \
  --output-dir build/ingestion-load/bfs-example
```

The controller chooses a source on a cross-backend edge, runs depth-two undirected BFS using generated runtime configuration, and compares exact IDs/distances with the normalized disk index. It writes `bfs.csv` and `bfs-verification.json`. Disconnected isolated data without cross-backend edges is reported accurately rather than inventing cross-backend coverage. The default validation cap is 100,000 reached vertices; `--bfs-max-vertices` raises it explicitly. `--bfs-binary` overrides the default sibling `polygraph_bfs` executable.

A retained environment can be checked afterward without reimporting:

```bash
python3 tools/ingestion/docker_load.py verify-bfs \
  --state build/ingestion-load/<generation>/environment.json \
  --bfs-binary build/cpu-tests/bin/polygraph_bfs
```

## Cleanup, cancellation and failure

Copy the exact `stop_remove_command` printed in `load-result.json`, or run:

```bash
python3 tools/ingestion/docker_load.py stop \
  --state build/ingestion-load/<generation>/environment.json
```

The controller checks exact container IDs and the generation ownership label before mutation. It checks network ownership too. It does not call Compose, global prune, delete arbitrary volumes, or act on existing service names. It recovers interrupted resource creation using the expected generated names plus ownership labels; missing already-removed resources are harmless. Cleanup is safe to repeat and retains artifact/log/storage directories. Bind-mounted database files may belong to the database's container UID; Docker cleanup does not change their ownership or delete them.

On failure, the command returns nonzero, records the failing phase/diagnostic, captures logs, and stops its owned resources while retaining their state for inspection. A failed native import remains uncertain: partial Arango/Neo4j writes and import journals are preserved. It does not erase journals, force overwrite, reuse a failed output directory, or retry imports into partially loaded stores. Use the printed cleanup command and start a fresh generation.

SIGINT/SIGTERM cancel readiness waits. During a native stage the entire local importer/helper process group is terminated before resource cleanup, preventing surviving Neo4j helper processes from restarting a server after cancellation. Logs and state remain available if the process is killed abruptly; the same cleanup command can recover owned resources afterward.

After the first cancellation signal, repeated SIGINT/SIGTERM are ignored until local child termination and state recording/cleanup finish. Cancelling `verify-load` records failed verification and keeps the services running; it performs no import cleanup or journal reset.

## Options

`--help` lists every option. In addition to the required adapter/input/placement and `--docker`, load supports:

- `--keep-running`, `--output-dir`, `--generation-name`, `--skip-verification`.
- Existing preparation limits: `--threads`, `--memory-limit-mb`, `--chunk-size-mb`, `--max-record-mb`, `--partitioner none`.
- `--concurrency` for native backend execution and `--startup-timeout-seconds` (default 180).
- `--arango-concurrency` (default 4, range 1–32), `--arango-batch-mb` (default 1, range 1–64) and `--arango-buffer-mb` (default 128, range 1–1024 and at least the batch target). These are independent of the backend concurrency limit. Set concurrency to 1 for serial Arango loading. At most eight batches wait in the queue; the byte budget includes active requests. Oversized records stay intact and must fit the buffer. The budget is for wire payloads, not whole-process memory.
- `--neo-image`, `--arango-image`, `--pg-image` for image overrides. Defaults reuse cached repository images; unavailable defaults fall back to official images. Pulling adds missing image content but does not replace running containers.
- `--verify-bfs`, `--bfs-binary`, `--bfs-max-vertices`.

The memory limit controls native preparation caches/records. The shared runner caps Neo4j importer off-heap memory to 256–2048 MiB from that budget, uses a 512 MiB admin heap, and starts the isolated Neo4j DBMS with a 512 MiB maximum heap / 128 MiB page cache. Arango uses a 64 MiB block cache and 32 MiB cache; PostgreSQL uses image defaults. These are disposable/import-oriented defaults, not production sizing recommendations.

## Regression and live tests

```bash
ctest --preset cpu-tests -R 'IngestionTest|python.ingestion' --output-on-failure

# Real public-CLI keep-running, cleanup and failed-native-import cases:
python3 tools/ingestion/docker_load_live.py --run \
  --binary build/cpu-tests/bin/polygraph_bulk_import \
  --bfs build/cpu-tests/bin/polygraph_bfs \
  --output-dir build/ingestion-docker-load/manual

# Opt-in CTest versions, including the existing refactored disposable harness:
cmake --preset cpu-tests -DPOLYGRAPH_INGESTION_LIVE_TESTS=ON
ctest --test-dir build/cpu-tests -L live --output-on-failure
```

Service-free tests cover ownership mismatches, credentials permissions/no secrets in argv, preparation failures, refusal to reuse output, interrupted container/network creation, cancellation propagation, process-group termination including a real grandchild, heterogeneous native-valid edge schemas and generated Arango tuning. Live tests verify imported counts/IDs/endpoints, actual cross-backend BFS, keep-running state, idempotent cleanup, failed native execution/journal retention and preservation of pre-existing containers. The parallel fixture imports 18,000 logical nodes and 18,000 edges, checks multiple concurrent Arango batches and their limits, then verifies exact native contents, Unicode properties and BFS without changing the import journal. The shared Docker lifecycle is also exercised by `tools/ingestion/disposable_live.py`.

## Original measured validation run

Before deferred verification was added on 2026-11-04, a full `load --docker --keep-running` completed against the unchanged `data/ldbc-snb/sf0_1/raw` root, using the three-backend placement configuration and cached repository images. No target configuration, credentials, database creation or Neo4j runner setup was supplied manually. All three services remained running after completion.

The logical snapshot contained **327,588 nodes and 1,477,965 relationships**. Strict artifact verification and both native execution validation and the final `verify-live` command passed, including exact canonical IDs, endpoints and traversal checks. Physical placement/replication counts were:

| Backend | Native nodes | Proxies | Relationships |
|---|---:|---:|---:|
| PostgreSQL | 9,486 | 304,352 | 318,353 |
| Neo4j | 15,278 | 293,383 | 772,198 |
| ArangoDB | 302,824 | 15,451 | 1,311,345 |

Preparation took **83.74 s**. Recorded native import durations were **12.63 s** for PostgreSQL COPY, **13.20 s** for Neo4j administrative import, and **214.50 s** for ArangoDB HTTP import. Full end-to-end elapsed time was **3,118.74 s (51m59s)**. Exhaustive live verification, especially ArangoDB endpoint checks, dominated elapsed time. That earlier workflow ran verification inside execution and again in an explicit final `verify-live` command. The duplicate live pass has since been removed, and `--skip-verification` now allows deferring the remaining checks. These are measurements from one development host, not a speedup claim or a comparison with the Python loader.

A separate `verify-bfs` invocation on the retained environment compared depth-two undirected PolyGraph BFS against the normalized graph. Root `563059475087361` reached **12,684 vertices**: 12,243 owned by ArangoDB, 349 by Neo4j and 92 by PostgreSQL. Every ID and distance matched.

The run's private results are under `build/ingestion-load/polygraph-load-20261104-135111-fc64e10d91/`: `load-result.json`, `artifacts/execution-state.json`, `artifacts/live-verification.json`, and `bfs-verification.json`. The default command does not request BFS, so its `load-result.json` records `bfs: null`; the separate BFS report records the completed check. These files and credentials remain ignored build output.

Final regression results: **497/497 CPU CTest tests**, **3/3 opt-in live ingestion CTest tests** (raw LDBC, canonical JSONL, public Docker load), **12 LDBC Python tests**, and **31 graph I/O Python tests** passed. The public Docker test covered keep-running, automatic removal, deliberate native-import failure, safe cleanup/retry refusal, and unchanged pre-existing container states.

The final service-free regression commands were:

```bash
cmake --preset cpu-tests -DPOLYGRAPH_INGESTION_LIVE_TESTS=OFF
cmake --build --preset cpu-tests --target polygraph_bulk_import polygraph_bfs polygraph_ingestion_test
ctest --preset cpu-tests --output-on-failure
POLYGRAPH_BULK_IMPORT="$PWD/build/cpu-tests/bin/polygraph_bulk_import" \
  python3 -m unittest tools.ingestion.test_docker_workflow tools.ingestion.test_semantics -v
python3 -m pytest tests/test_ldbc_snb_conversion.py tests/test_ldbc_fast_prepare.py -q
python3 -m unittest tools.graphio.test_cache_planner tools.graphio.test_loaders \
  tools.graphio.test_gpu_uuid_routing tools.graphio.test_domain_manifest
clang-format-21 --dry-run --Werror app/bulk_import/main.cpp
git diff --check
```

## Deferred-verification measurement

The new import-only path was tested on the same unchanged SF0.1 input and three-backend placement:

```bash
build/cpu-tests/bin/polygraph_bulk_import load \
  --adapter ldbc-snb --input data/ldbc-snb/sf0_1/raw \
  --placement-config tools/ldbc_snb/placement.schema-3db.json \
  --docker --keep-running --skip-verification \
  --output-dir build/ingestion-deferred/sf01-fast-001
```

It completed in **311.24 s (5m11s)**, including **87.66 s** preparation and **215.26 s** native execution. Backend import measurements were PostgreSQL **1.68 s**, Neo4j administrative import **14.42 s**, and ArangoDB **215.25 s**; the backends execute concurrently. All three services remained running. The logical manifest contains the same 327,588 nodes and 1,477,965 relationships, with the same physical placement/replication counts as above. `load-result.json` correctly records `imported`, `verification: skipped`, and `counts_source: manifest`; no strict-reconstruction or exhaustive live-verification stage ran.

A separate read-only BFS check on these imported databases matched all 12,684 depth-two vertex IDs/distances across the three backends. This checks the reached subgraph; it does not mark the full dataset verified. Full standalone `verify-load` was exercised on fresh disposable fixture databases, including exact verification/BFS success, deliberate live-document corruption detection, unchanged import journals and running services after failure, and success after restoring the fixture. The new standalone full verifier was not rerun over the entire SF0.1 dataset during this change. To request that exhaustive check explicitly:

```bash
build/cpu-tests/bin/polygraph_bulk_import verify-load \
  --output-dir build/ingestion-deferred/sf01-fast-001 --verify-bfs
```

Import-only and fully verified durations describe different workloads: verification work has been deferred, not made free. These measurements do not establish a like-for-like throughput speedup. The new real-data output, private credentials, import journal, BFS report and logs remain under the indicated ignored build directory.

The final deferred-verification change passed **501/501 ordinary CPU CTest tests**, **3/3 opt-in live CTest tests**, **23 Docker/semantics Python tests**, **12 LDBC Python tests**, and **31 graph I/O Python tests**. The live Docker regression now exercises four modes (verified keep-running, verified removal, failed import-only execution, and import-only followed by separate correctness verification). The combined regression coverage also checks target binding, stale-result invalidation and unchanged import timing/journals. The live run preserved all 42 pre-existing container IDs plus all 33 non-ingestion container states. The complete live suite took 705.04s on the shared development host; Docker lifecycle timing varied between runs.
