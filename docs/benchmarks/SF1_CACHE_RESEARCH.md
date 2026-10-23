# SF1 cache-placement benchmark

This benchmark compares PolyGraph cache placement on one fixed, schema-based
LDBC SNB distribution across Neo4j, ArangoDB, and GPU. PostgreSQL is intentionally
not part of this experiment. Redis and memory mapping are cache tiers; GPU is both
an authoritative schema-placement target for selected types and an acceleration/cache
tier according to the selected cache plan.

## Cache configurations

The configurations in `config/cache/benchmark/sf1/` are ordered so shell globbing
runs them in a stable order:

1. `01-gpu-100.json`: every vertex cached on GPU.
2. `02-redis-100.json`: every vertex cached in Redis.
3. `03-mmap-100.json`: every vertex cached in the mmap artifact.
4. `04-gpu30-redis20-mmap20.json`: hottest 30% GPU, next 20% Redis,
   next 20% mmap, remaining 30% uncached.
5. `05-gpu20-redis15-mmap15.json`: hottest 20% GPU, next 15% Redis,
   next 15% mmap, remaining 50% uncached.
6. `06-redis-25.json`: hottest 25% Redis, remaining 75% uncached.
7. `07-mmap-25.json`: hottest 25% mmap, remaining 75% uncached.
8. `08-no-cache.json`: no acceleration cache.

Percentages are percentages of vertices, not bytes or relationships. Fractional
policies rank vertices globally by degree. Mixed configurations use non-overlapping
rank windows. Redis already excludes GPU-resident vertices when
`shared_hot_fraction` is zero, while mmap uses an explicit rank start.

The 100% GPU configuration deliberately has no planner memory cap. If SF1 does
not fit the physical GPU, that configuration is infeasible on that machine and
should be reported as such rather than silently changing the experiment.

## Fast preparation

For a clean cache experiment, prepare canonical IDs with one deterministic
schema-based placement shared by every cache configuration. The supplied placement
maps complete logical types to Neo4j, ArangoDB, or GPU:

```bash
/usr/bin/time -v -o measurements/sf1-cache-paper/prepare.time.txt \
python3 tools/polygraph.py prepare ldbc \
  --input data/ldbc-snb/sf1_0/raw/social_network-sf1-CsvBasic-LongDateFormatter \
  --sf 1.0 \
  --domain-id ldbc-snb-sf1_0-cache-paper \
  --output-dir data/ldbc-snb/sf1_0/cache-paper \
  --placement-config tools/ldbc_snb/placement.sf1-schema-neo-arango-gpu.json \
  --storage-backends Neo4J ArangoDB GPU \
  --root-count 100 \
  --seed 42 \
  --max-depth 7 \
  --direction both \
  --validation-mode basic \
  --no-placement-graph-render
```

`basic` validation reuses bounded-memory checks performed during conversion and
avoids the expensive exact second graph validation pass. Skipping Graphviz
rendering removes presentation-only work from dataset preparation. The JSONL
writer uses a multi-megabyte output buffer.

## Start only Neo4j and ArangoDB

The instance commands accept a service subset so the PostgreSQL service does not
consume resources during this study:

```bash
python3 tools/polygraph.py instance sf1_0 schema-based up \
  --services neo4j arangodb
```

Start Redis separately with a benchmark-sized no-eviction memory limit:

```bash
docker network inspect polygraph-observability >/dev/null 2>&1 || \
  docker network create polygraph-observability

docker compose \
  --env-file storage_layer/redis/.env.sf1-benchmark \
  -f storage_layer/redis/docker-compose.yml \
  up -d redis
```

## Fast database load

The CLI exposes the existing Arango `_api/import` worker pool. SF1 is usually a
good size for a larger import batch and multiple workers:

```bash
/usr/bin/time -v -o measurements/sf1-cache-paper/load.time.txt \
python3 tools/polygraph.py instance sf1_0 schema-based load \
  data/ldbc-snb/sf1_0/cache-paper/domain.json \
  --services neo4j arangodb \
  --only-source Neo4J \
  --only-source ArangoDB \
  --batch-size 50000 \
  --workers 8
```

Increase `--workers` only while load throughput improves; using more workers can
reduce performance if ArangoDB or storage becomes saturated. The loader already
uses the Neo4j Bolt path when configured and parallel Arango import workers.

## HTTP request schedule

`config/benchmarks/sf1-cache-http-requests.example.json` is directly runnable and
uses the domain's default root. A request can override `startUuid` with any
canonical uint64 PolyGraph ID and can override any HTTP BFS payload field.

`atMs` is relative to the time `/healthz` reports the server ready. Requests are
strictly serial for benchmarking and exact event attribution. Therefore `atMs`
is an earliest start time: if an earlier BFS is still running, the next request
starts late and `schedule_delay_ms` records the lateness. Set `"measure": false`
for warmups; those rows and event files are retained, but they are excluded from
the aggregate mean/min/max comparison statistics.

For a paper, replace or extend the request list with the fixed roots from
`data/ldbc-snb/sf1_0/cache-paper/start_uuids.txt` and use exactly the same request
plan for every cache configuration.

## Run the complete cache comparison

Build first:

```bash
python3 tools/polygraph.py build --preset gpu-bfs
```

Then generate every cache plan, materialize Redis when needed, restart the HTTP
server for every cache plan, execute the same request schedule, and store the
results:

```bash
python3 tools/polygraph.py benchmark-caches \
  data/ldbc-snb/sf1_0/cache-paper/domain.json \
  --transport http \
  --cache-configs config/cache/benchmark/sf1/*.json \
  --http-request-plan config/benchmarks/sf1-cache-http-requests.example.json \
  --cache-root data/ldbc-snb/sf1_0/cache-paper/caches/paper \
  --planning-db data/ldbc-snb/sf1_0/cache-paper/cache-plan.sqlite3 \
  --output-dir measurements/sf1-cache-paper/cache-comparison \
  --redis-batch-size 50000 \
  --overwrite
```

The first cache generation builds `cache-plan.sqlite3`; subsequent configurations
reuse it instead of rescanning/reindexing `graph.jsonl`.

Each cache directory in the measurement output contains:

```text
measurements.csv
summary.json
requests/<request-id>.json
outputs/requests/<request-id>.json
events/server.events.jsonl
events/requests/<request-id>.events.jsonl
events/requests/index.json
server/command.txt
server/drogon.json
server/requests.json
logs/server.log
```

The root comparison directory also contains `comparison.csv`, `comparison.json`,
`comparison.md`, `benchmark.json`, and `benchmark.timeline.jsonl`.

The per-request event files are split after server shutdown using the event's own
`timestamp_unix_ns` and the exact HTTP request interval. The complete server event
log is retained as well.

## Benchmark methodology

Use one immutable prepared graph, one root/request schedule, one BFS payload, and
one backend database state for every cache configuration. The runner starts a new
PolyGraph HTTP process for every cache plan, while Neo4j and ArangoDB stay alive.
This makes the experiment primarily a cache-placement/runtime comparison rather
than a database cold-start comparison.

Choose one cache-state methodology and report it explicitly:

- **Warm persistent backends:** warm up first, then compare measured HTTP requests.
  This is the recommended primary cache-placement experiment.
- **Cold system experiment:** restart databases and clear relevant OS caches between
  runs. Treat this as a separate experiment because it measures a different thing.

The example request plan marks its first call with `"measure": false`, so the raw
warmup measurement remains available without contributing to aggregate reported
steady-state statistics.


## Backend request scheduling

HTTP benchmark runs share server-global Neo4j and ArangoDB schedulers. Configure hard limits in each backend's `scheduler` object and use `backendSchedulerPriority` / `backendSchedulerWeight` per scheduled HTTP request. See `docs/BACKEND_REQUEST_SCHEDULING.md`.
