# Bounded concurrent native ingestion

Measured on **2026-11-05**, using the unchanged LDBC SNB SF0.1 raw snapshot and three-backend placement. Two fresh runs with one ArangoDB worker were interleaved with two fresh runs with four workers. The executable, source fingerprint, input hashes, images, 1 MiB batch target, 128 MiB payload budget, three preparation threads and 256 MiB preparation budget were identical. Only ArangoDB worker count changed. All native artifact paths, checksums and record counts also matched across trials. The [machine-readable comparison](data/native-ingestion-concurrency-sf0_1-2026-11-05.json) records every trial, input/artifact identities, phase timings, resource counters, sample variation and correctness results.

The snapshot has **327,588 nodes and 1,477,965 relationships**, producing **3,342,670 physical records** after placement, proxies and relationship replication. ArangoDB imports 1,629,620 of those records in 297 acknowledged batches in every trial. This comparison uses a **Debug** CPU build with GCC 11.4 on the same Ryzen 7 5800H / 16 logical CPU / 31.20 GiB host as the [original baseline](INGESTION_SF0_1_BASELINE.md), with a `powersave` governor and Docker 29.1.3. Cached Neo4j 5.26 Enterprise, ArangoDB 3.12.11 and PostgreSQL 17 image IDs are recorded with the data.

## Timings

| Trial | Arango workers | Preparation | Concurrent native execution | Whole Docker load |
|---|---:|---:|---:|---:|
| Serial 1 | 1 | 85.853 s | 214.959 s | 310.060 s |
| Parallel 1 | 4 | 87.583 s | 62.933 s | 159.860 s |
| Serial 2 | 1 | 86.412 s | 207.325 s | 302.990 s |
| Parallel 2 | 4 | 86.713 s | 61.241 s | 157.300 s |
| Serial mean | 1 | 86.133 s | **211.142 s** | **306.525 s** |
| Parallel mean | 4 | 87.148 s | **62.087 s** | **158.580 s** |

The mean native execution time decreased **70.6% (3.40×)** and whole-load time decreased **48.3% (1.93×)**. Preparation was unchanged. This is a measured same-work comparison of serial versus concurrent execution, rather than a comparison of verified and unchecked imports. All timed loads deferred exhaustive verification; checksum, endpoint resolution, fresh-target checks, native constraints, indexes and acknowledgements remained enabled.

Native throughput based on mean duration rose from **15,831 to 53,839 physical records/s**. Whole-load throughput rose from **5,890 to 11,386 logical entities/s**. Physical records include proxies and replicated edges; logical entities are input nodes plus relationships.

Native execution includes overlapping PostgreSQL COPY, Neo4j offline import/startup/index work, and ArangoDB collection/import/index work. Preparation's internal timer excludes its final checksum/publication pass; whole-load GNU time includes preparation, provisioning and loading, excluding later correctness verification, resource-report collection and cleanup. Phase durations therefore do not sum to whole-load time.

## What changed and why

The original baseline identified serial ArangoDB import as the critical path: 224.104 s out of 224.107 s native execution. The new phase measurements confirm that edge import dominates. Across the two repetitions, mean ArangoDB edge import fell from **203.394 s to 55.076 s**. Document/proxy import and UUID index construction are measured separately.

The C++ executor now streams whole JSONL records into a fixed `std::jthread` pool. Defaults are four workers, 1 MiB target batches, eight queued batches and 128 MiB of queued plus in-flight wire payloads. The observed concurrent peak was four active requests, eight queued batches and **12,582,129 payload bytes**. Serialization and HTTP calls run outside the queue mutex. All document/proxy acknowledgements complete before edges begin; all edges complete before UUID indexes are created. Failed acknowledgements discard pending work, join active requests and fail the existing execution journal; uncertain writes are never retried automatically.

PostgreSQL retains transactional libpq COPY and the existing indexes/durability. Neo4j retains fresh offline `neo4j-admin` import, safe startup/registration and required indexes/property repairs. Neither was the bottleneck, so their import mechanisms were preserved. New timers separate PostgreSQL COPY/index/analyze/commit and Neo4j admin/startup/index/repair phases rather than attributing every backend's elapsed time to its importer.

| Backend phase, mean elapsed | One Arango worker | Four Arango workers |
|---|---:|---:|
| PostgreSQL complete import | 1.928 s | 2.129 s |
| Neo4j admin import only | 14.786 s | 16.216 s |
| Neo4j complete offline/startup/index lifecycle | 36.412 s | 43.741 s |
| ArangoDB documents/proxies | 4.261 s | 1.359 s |
| ArangoDB edges | 203.394 s | 55.076 s |
| ArangoDB UUID indexes | 3.405 s | 5.588 s |

The unchanged backends and index building were somewhat slower while more Arango work ran concurrently; these are overlapping shared-host measurements, not isolated backend speedup tests. ArangoDB still determines the native critical path. Preparation now takes longer than the native execution phase; optimizing preparation or Neo4j startup would be separate measured work.

## Resource cost

These are means of each trial's measurement, including mean sampled peaks rather than an instantaneous combined host peak:

| Metric | One worker | Four workers |
|---|---:|---:|
| Client process-family CPU | 114.565 CPU s | 116.725 CPU s |
| Maximum client process RSS | 224.010 MiB | 223.777 MiB |
| Native executor maximum RSS | 35.141 MiB | 34.955 MiB |
| ArangoDB cumulative CPU through last sample | 224.795 CPU s | 240.664 CPU s |
| ArangoDB sampled peak cgroup memory | 1.484 GiB | 1.532 GiB |
| Peak sample-round sum of owned cgroup memory | 2.955 GiB | 3.156 GiB |

Faster loading uses more simultaneous database work. ArangoDB CPU increased about 7.1% on these sampled lifetime counters, while mean sampled memory peak increased about 3.2%. Client RSS stayed near 224 MiB. The wire-payload budget does not cap database memory or total client RSS; producer buffers, string capacity, HTTP responses and server allocations require additional memory.

Docker was sampled every two seconds. Cgroup memory includes page cache/kernel usage and can miss between-sample peaks; a round sums sequential readings. Client CPU excludes container/daemon CPU, and maximum process RSS is not summed concurrent RSS. Container CPU covers its observed lifetime through the last sample, including startup; the auto-removed Neo4j importer can lose its final CPU tail. All four included trials had zero monitoring errors.

Mean allocated artifacts plus SQLite were **789.000 MiB serial / 789.002 MiB parallel**. Final database bind allocation was **1.638 GiB / 1.677 GiB** respectively, measured with `du -sk` inside the owned running containers before verification. These are final allocated blocks, not peak temporary disk usage or disk I/O; differently timed WAL/compaction can affect them. Serial trial 2 omitted one incomplete exiting-container resource observation (`unavailable_container_samples: 1`); all other included trials recorded zero unavailable observations.

## SF0.1 correctness

The final optimized trial passed strict artifact reconstruction and exhaustive live canonical ID/type/proxy/relationship endpoint checks against all three fresh databases. The exact live counts were:

| Backend | Native nodes | Proxies | Relationships |
|---|---:|---:|---:|
| PostgreSQL | 9,486 | 304,352 | 318,353 |
| Neo4j | 15,278 | 293,383 | 772,198 |
| ArangoDB | 302,824 | 15,451 | 1,311,345 |

Actual distributed CPU BFS from `563059475087361` at depth two matched **12,684 canonical vertex IDs and distances**: 12,243 owned by ArangoDB, 349 by Neo4j and 92 by PostgreSQL. The complete correctness/BFS command took **1,285.703 s (21m25.7s)** outside the timed import. The historical import journal remained `imported`, `verification: skipped`; separate verification recorded `verified` without replaying any import. The collector published its benchmark only after verification succeeded and then removed its owned containers/network, retaining private artifacts and logs.

## Regression coverage

The ordinary CPU suite passed **511/511 CTest cases**. Focused Python runs passed **33 ingestion**, **12 LDBC conversion/preparation** and **31 graph-I/O** tests. The eight new batch cases passed 100 repetitions normally, under Clang 21 ThreadSanitizer and under GCC 11 AddressSanitizer/UBSan. GCC 11's TSan runtime failed at startup with `unexpected memory mapping`; Clang's runtime successfully ran the same tests. Sanitizer coverage is focused on the scheduler/framer, rather than instrumenting database client libraries or the complete engine.

The public disposable Docker suite passed all five modes: keep-running, automatic removal, native failure/retry refusal, deferred verification/corruption detection, and parallel import. The parallel fixture contains 18,000 nodes and 18,000 edges. It checks concurrent batching and limits, all three databases' exact IDs/counts/endpoint traversal, sampled Unicode properties, actual cross-backend BFS and an unchanged import journal after verification. Existing container identities and running states were preserved.

```bash
ctest --preset cpu-tests --output-on-failure
POLYGRAPH_BULK_IMPORT="$PWD/build/cpu-tests/bin/polygraph_bulk_import" \
  python3 -m unittest tools.ingestion.test_docker_workflow \
    tools.ingestion.test_semantics tools.ingestion.test_benchmark_ldbc -v
python3 -m pytest tests/test_ldbc_snb_conversion.py tests/test_ldbc_fast_prepare.py -q
python3 -m unittest tools.graphio.test_cache_planner tools.graphio.test_loaders \
  tools.graphio.test_gpu_uuid_routing tools.graphio.test_domain_manifest
python3 tools/ingestion/docker_load_live.py --run \
  --binary build/cpu-tests/bin/polygraph_bulk_import \
  --bfs build/cpu-tests/bin/polygraph_bfs \
  --output-dir build/ingestion-optimization/live
```

The live-suite report is `build/ingestion-optimization/live/run-fb51019486f7/result.json`; ordinary test and sanitizer logs are under `build/ingestion-optimization/`. Changed C++ files passed `clang-format-21 --dry-run --Werror`; Python compilation and `git diff --check` passed. The full clang-tidy suite was not run.

## Reproduce

```bash
cmake --preset cpu-tests -DPOLYGRAPH_INGESTION_LIVE_TESTS=OFF
cmake --build --preset cpu-tests --target polygraph_bulk_import polygraph_bfs

for trial in 1 2; do
  for workers in 1 4; do
    python3 tools/ingestion/benchmark_ldbc.py \
      --binary build/cpu-tests/bin/polygraph_bulk_import \
      --input data/ldbc-snb/sf0_1/raw \
      --placement-config tools/ldbc_snb/placement.schema-3db.json \
      --scale-factor 0.1 --docker --threads 3 --memory-limit-mb 256 \
      --arango-concurrency "$workers" --arango-batch-mb 1 --arango-buffer-mb 128 \
      --output-dir "build/ingestion-benchmark/sf01-workers-${workers}-trial-${trial}"
  done
done
```

For full correctness and BFS on an optimized trial, add `--verify-after --bfs-binary build/cpu-tests/bin/polygraph_bfs` to that invocation. Verification happens outside timing before owned resources are removed. Each output directory must be new. Raw measurements, logs, source snapshots, private credentials and retained storage for these trials live under ignored `build/ingestion-optimization/comparison-002/`. Existing containers, volumes and Compose projects were never reused or modified.

The loader exposes the same settings as `load --arango-concurrency 4 --arango-batch-mb 1 --arango-buffer-mb 128`; manual targets use `import_concurrency`, `import_batch_mb` and `import_buffer_mb`. See [native ingestion](../guides/BULK_INGESTION.md) for payload bounds, record limits and recovery, and [benchmarking](../guides/BENCHMARKING.md) for collector scopes and missing-observation reporting.

## Limits

Only two repetitions per variant at SF0.1 were measured on a shared development host with uncontrolled OS/database caches and nearly full swap. These results establish a local baseline; they do not establish significance, an optimal thread/batch setting, SF1/SF10 scaling or a native-versus-Python loading speedup. Increasing concurrency can increase server CPU and memory pressure.

An earlier four-worker/8 MiB pilot was aborted by a Docker monitoring timeout during host memory/I/O pressure. A later serial trial was aborted when a departing Neo4j admin container supplied incomplete counters. Neither produced a successful benchmark and neither is included above. Monitoring now records transient timeouts and unavailable counters explicitly instead of cancelling a healthy import. Failed imports retained diagnostics and were not replayed into uncertain stores.
