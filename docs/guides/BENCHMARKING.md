# Benchmarking PolyGraph

This page documents repository tooling. The separate `docs/benchmarks/` thesis material is intentionally left untouched.

## Native ingestion baseline

The [LDBC SNB SF0.1 ingestion baseline](../benchmarks/INGESTION_SF0_1_BASELINE.md) publishes one measured import-only data point with preparation/native/end-to-end times, throughput, client CPU/RSS, sampled Docker resources and final disk allocation. It includes a [machine-readable record](../benchmarks/data/native-ingestion-sf0_1-2026-11-04.json), exact reproduction commands and the measured bottleneck. `tools/ingestion/benchmark_ldbc.py` measures a real raw dataset using fresh owned Docker targets; increasing scale factors and repeated trials can use the same tool with different input labels and fresh output directories. A single SF0.1 observation does not establish a scaling curve.

The [concurrent ingestion comparison](../benchmarks/INGESTION_THROUGHPUT.md) records two matched SF0.1 runs per worker setting and explains the measured bottleneck.

To compare serial and concurrent Arango loading, use the same executable, input, placement, images and batch/buffer sizes, changing only worker count. Each trial needs a fresh output directory:

```bash
for trial in 1 2 3; do
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

Add `--verify-after --bfs-binary build/cpu-tests/bin/polygraph_bfs` to a representative optimized trial to run strict artifact reconstruction, exact native ID/type/endpoint verification and cross-backend BFS **after** stopping the timing/resource measurements. The collector retains its owned services until verification completes, publishes `post_import_verification` only on success, and cleans up only those resources. A failed or interrupted import is never replayed; retain diagnostics and use a new directory. Exhaustive verification can take substantially longer than the import.

Reports include the executable SHA-256, engine-source fingerprint, collector SHA-256, git revision/dirty state, input hashes, image IDs and host pressure indicators. Match these before attributing a difference to worker count. Arango reports document, edge and index times plus acknowledged batches and observed queue/payload limits; PostgreSQL and Neo4j report their native phases separately. Compare repeated trials, variation and resource costs, not only the fastest time.

Monitoring is observational: a transient Docker stats timeout is recorded as `containers.sampling_errors` and does not cancel a healthy load. Starting/exiting containers with incomplete CPU/memory counters are counted in `unavailable_container_samples`; these observations are omitted, never fabricated as zero. Resource figures remain sampled lower bounds, including the auto-removed Neo4j importer's CPU tail. Exclude failed trials and flag incomplete monitoring when comparing resource costs. Final disk allocation is measured before verification/cleanup; neither verification nor cleanup belongs to the timed import.

## BFS benchmark executable

The dedicated executable initializes the runtime once and runs repeated BFS measurements:

```bash
./build/gpu-bfs/bin/polygraph_bfs_benchmark \
  --domain data/ldbc-snb/sf10_0/schema-based/domain.json \
  --cache-plan data/ldbc-snb/sf10_0/schema-based/caches/cache-a/cache-plan.json \
  --start-uuid "$START_UUID" \
  --repetitions 5 \
  --max-depth 10 \
  --direction both \
  --output-dir measurements/sf10-cache-a \
  --overwrite
```

Runtime initialization is outside the per-repetition BFS timing and is recorded separately. Operational startup/GPU-loading logs are installed before runtime construction, so a large projection no longer looks like a hung process.

## Compare multiple cache configurations

Use the dedicated comparison script when the experimental variable is cache policy:

```bash
python3 tools/polygraph.py benchmark-caches data/ldbc-snb/sf10_0/schema-based/domain.json \
  --cache-configs \
    config/cache/cache-a.json \
    config/cache/cache-b.json \
    config/cache/cache-c.json \
  --start-uuid "$START_UUID" \
  --repetitions 10 \
  --max-depth 10 \
  --direction both \
  --output-dir measurements/sf10-cache-comparison \
  --overwrite
```

The cache-config list is ordered and may contain any number of JSON files. The script:

1. reuses one `cache-plan.sqlite3` graph/index DB;
2. generates an independent directory and `residency.bin` for each config;
3. materializes Redis when enabled unless `--skip-redis-load` is used;
4. invokes `polygraph_bfs_benchmark --cache-plan ...`;
5. writes an incremental `comparison.csv`, `comparison.json`, and a readable `comparison.md`.

Common options and defaults:

| Option | Default | Purpose |
|---|---:|---|
| `--repetitions` | `5` | BFS repetitions per selected start ID |
| `--max-depth` | `10` | BFS depth |
| `--direction` | `both` | traversal direction |
| `--cooldown-ms` | `0` | delay between repetitions |
| `--redis-batch-size` | `10000` | Redis materialization batch size |
| `--cache-root` | `<domain>/caches` | generated plan directories |
| `--planning-db` | beside `graph.jsonl` | reusable graph/index DB |

Use `--start-uuids-file` for multiple roots. Extra options after `--` are forwarded to the selected BFS transport.

### Benchmark through the HTTP server

Use `--transport http` when the measured path should be the real Drogon BFS endpoint rather than the in-process benchmark executable:

```bash
python3 tools/polygraph.py benchmark-caches data/ldbc-snb/sf10_0/schema-based/domain.json \
  --transport http \
  --cache-configs config/cache/*.json \
  --start-uuids-file data/ldbc-snb/sf10_0/schema-based/start_uuids.txt \
  --repetitions 5 \
  --output-dir measurements/sf10-all-caches-http \
  --overwrite \
  -- \
  --no-predecessors \
  --neo4j-max-concurrent-requests 16 \
  --arango-max-concurrent-requests 16
```

For every cache configuration the runner generates/materializes the cache, starts a fresh `polygraph_http_server` with that `cache-plan.json`, waits for `/healthz`, executes every root/repetition with `POST /api/v1/graphs/<source>/bfs`, and stops the server before the next cache configuration. Server startup time therefore includes runtime and GPU projection loading and is recorded separately from request latency.

HTTP mode defaults to port `8080`, matching `observability/prometheus/prometheus.yml`. This keeps PolyGraph process/Jemalloc/GPU/HTTP metrics scrapeable while Neo4j, ArangoDB, Redis and cAdvisor continue to be measured by the same observability stack. If `--http-port` is changed, update the Prometheus target as well.

The HTTP request supports the native BFS tuning controls used by cache experiments: `--max-depth`, `--direction`, `--allowed-edge-types`, `--allowed-target-node-types`, `--allow-native-pushdown`, `--no-partial-acceleration`, `--no-predecessors`, `--neighbor-batch-size`, `--neo4j-max-concurrent-requests`, `--arango-max-concurrent-requests`, `--backend-request-retries`, `--backend-retry-delay-ms`, and `--thread-count`. Unsupported binary-only arguments are rejected instead of silently ignored.

`--http-response-mode summary` is the default for performance studies: the request still crosses the real HTTP stack, but the response contains counts and server-side BFS timing instead of serializing/transferring the complete distance map. Use `--http-response-mode full` when the full API response cost is itself part of the benchmark.

Each cache run contains `measurements.csv`, `server/drogon.json`, `server/request.json`, `logs/server.log`, `events/server.events.jsonl`, and response files under `outputs/`. The comparison root also contains `benchmark.timeline.jsonl`, whose nanosecond timestamps mark cache/server/request phase boundaries for correlation with Prometheus/Grafana data.


### Reuse already-generated cache plans

When the cache artifacts already exist, use `--cache-plans` instead of `--cache-configs`. This path validates the selected `cache-plan.json`, its `residency.bin`, and any MMAP/Redis binary artifacts, then starts the benchmark without running cache planning or rewriting those cache directories.

```bash
python3 tools/polygraph.py benchmark-caches data/ldbc-snb/sf10_0/schema-based/domain.json \
  --transport http \
  --cache-plans \
    data/ldbc-snb/sf10_0/schema-based/caches/cache-a/cache-plan.json \
    data/ldbc-snb/sf10_0/schema-based/caches/cache-b/cache-plan.json \
  --start-uuids-file /tmp/bfs-start-uuids.txt \
  --repetitions 5 \
  --output-dir measurements/sf10-existing-caches \
  --overwrite \
  -- \
  --no-predecessors \
  --neo4j-max-concurrent-requests 16 \
  --arango-max-concurrent-requests 16
```

`--cache-configs` and `--cache-plans` are mutually exclusive:

- `--cache-configs`: generate cache artifacts, optionally materialize Redis, then benchmark;
- `--cache-plans`: reuse existing disk cache artifacts, optionally materialize Redis, then benchmark.

Redis is external process state, not a file inside the cache directory. Therefore existing plans still materialize Redis by default when the plan contains a Redis artifact. Add `--skip-redis-load` only when the exact selected plan is already present in Redis and its readiness marker is valid. GPU memory is always rebuilt when the fresh HTTP server process starts.

## What to compare

A useful cache study should keep the canonical graph, persistent backend contents, BFS roots, BFS options, machine state, and build constant while changing only cache configuration. At minimum report:

- runtime/projection initialization time;
- per-run BFS duration and distribution, not only the mean;
- discovered vertex count/result equivalence;
- GPU and Redis resident vertex/edge counts;
- Redis hit/use behavior from events/metrics;
- GPU memory and host memory footprint;
- persistent backend request counts/time;
- cache-plan generation time separately from query runtime.

For fair cache comparisons, regenerate/materialize each plan from the same immutable graph. Do not compare a new plan against stale Redis contents.

## General benchmark suite

The broader benchmark orchestrator remains available through:

```bash
python3 tools/polygraph.py benchmark path/to/suite.json
```

Useful flags include `--run-id`, `--output-dir`, `--dry-run`, `--skip-build`, `--skip-services`, and `--resume`.
