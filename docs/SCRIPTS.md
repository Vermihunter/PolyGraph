# Scripts and command-line workflows

`tools/polygraph.py` is the main operator-facing entry point. Run any command with `--help` to see the exact current arguments.

## Most common workflow

```bash
# Build
python3 tools/polygraph.py build --preset gpu-bfs

# Prepare canonical data
python3 tools/polygraph.py prepare ldbc \
  --input <extracted-CsvBasic-directory> \
  --sf 10.0 \
  --domain-id ldbc-snb-sf10_0-schema-based \
  --output-dir data/ldbc-snb/sf10_0/schema-based \
  --placement-config data/ldbc-snb/placement.config.json \
  --root-count 100

# Start storage
python3 tools/polygraph.py services start

# Load persistent backends
python3 tools/polygraph.py load data/ldbc-snb/sf10_0/schema-based/domain.json --reset

# Build independent cache configurations
python3 tools/polygraph.py cache build data/ldbc-snb/sf10_0/schema-based/domain.json \
  --config config/cache/cache-a.json config/cache/cache-b.json

# Materialize the plan selected for this run
python3 tools/polygraph.py cache redis-load data/ldbc-snb/sf10_0/schema-based/domain.json \
  --cache-plan data/ldbc-snb/sf10_0/schema-based/caches/cache-a/cache-plan.json

# Execute repeated BFS
python3 tools/polygraph.py run data/ldbc-snb/sf10_0/schema-based/domain.json \
  --start-uuid "$START_UUID" --repetitions 5 --run-id sf10-a -- \
  --cache-plan data/ldbc-snb/sf10_0/schema-based/caches/cache-a/cache-plan.json \
  --max-depth 10 --direction both
```

## `prepare ldbc`

```bash
python3 tools/polygraph.py prepare ldbc --input DIR [options]
```

Common options:

- `--sf FLOAT` — scale factor; inferred when possible.
- `--domain-id NAME` — stable domain identifier.
- `--output-dir DIR` — prepared domain location.
- `--placement-config FILE` — authoritative data placement.
- `--storage-backends Neo4J ArangoDB PostgreSQL GPU` — ordered owner/routing layout (use only the owners desired for that dataset).
- `--root-count N` — number of generated BFS roots.
- `--seed N` — deterministic root/placement seed where applicable.
- `--max-depth N` — domain BFS default.
- `--direction directed|undirected|both` — domain BFS default.
- `--arango-database NAME` — generated ArangoDB database.

`--cache-config` remains accepted for compatibility, but the recommended design is to prepare/load first and use `cache build` afterwards so data and cache experiments remain independent.

## `generate ldbc`

```bash
python3 tools/polygraph.py generate ldbc --sf 1.0 --output-dir ...
```

In addition to preparation options, generation accepts `--tag`, `--memory`, `--update-partitions`, `--base-image`, and `--force` for the LDBC generator container.

## `build`

```bash
python3 tools/polygraph.py build --preset gpu-bfs
```

Typical presets are `cpu-core`, `cpu-bfs`, `cpu-bfs-debug`, `gpu-bfs`, `gpu-bfs-debug`, `http-server`, `http-server-observability`, `grpc-server`, `full-cpu`, and `docs`.

## `load`

```bash
python3 tools/polygraph.py load DOMAIN [--reset] [--batch-size 20000]
```

- `--reset` recreates/clears selected persistent data before loading.
- `--batch-size` defaults to `20000`.
- `--only-source SOURCE` can be repeated, for example `Neo4J`, `ArangoDB`, or `PostgreSQL`.

Long stages report progress approximately every five seconds.

## `cache build`

```bash
python3 tools/polygraph.py cache build DOMAIN \
  --config CACHE_A.json CACHE_B.json ...
```

Common options:

- `--output-dir DIR` — default `<domain>/caches`.
- `--planning-db FILE` — default `<graph-directory>/cache-plan.sqlite3`.
- `--load-redis` — materialize Redis immediately.
- `--redis-no-reset` — preserve current Redis keys rather than safe reset.
- `--redis-batch-size N` — default `10000`.

Every config gets a separate plan directory; the SQLite graph/index database is shared.

## `cache redis-load`

```bash
python3 tools/polygraph.py cache redis-load DOMAIN \
  --cache-plan CACHE/cache-plan.json \
  --batch-size 10000
```

Reset is the default. `--no-reset` is an explicit opt-out.

The loader removes the readiness marker first and publishes it only after full materialization. Runtime Redis use is disabled when the marker is absent or belongs to another cache plan.

## `run`

```bash
python3 tools/polygraph.py run DOMAIN [wrapper options] -- [BFS executable options]
```

Wrapper options:

- `--run-id NAME` — default `<domain>-<UTC timestamp>`.
- `--start-uuid ID` or `--start-uuids-file FILE`.
- `--repetitions N` — default `1`.
- `--overwrite-logs` — replace an existing run's artifacts.
- `--thread-count N`.
- `--neighbor-batch-size N`.
- `--binary FILE` — override executable discovery.

The wrapper owns event/log/result paths. Do not pass `--event-file` manually through the trailing BFS arguments.

Per repetition it invokes the BFS binary with:

```text
--event-file       measurements/<run>/events/<id>/<rep>.events.jsonl
--log-file         measurements/<run>/logs/<id>/<rep>.log
--output-file      measurements/<run>/outputs/<id>/<rep>.csv
```

Operational logs remain visible on stdout while CSV output stays clean.

Useful forwarded BFS options include:

- `--cache-plan FILE`
- `--max-depth N`
- `--direction directed|undirected|both`
- `--allowed-edge-types CSV`
- `--allowed-target-node-types CSV`
- `--allow-native-pushdown`
- `--no-partial-acceleration`
- `--no-predecessors`
- `--neo4j-max-concurrent-requests N`
- `--arango-max-concurrent-requests N`
- `--backend-request-retries N`
- `--backend-retry-delay-ms N`

## Direct BFS CLI output/logging options

```text
--output-file FILE        BFS distance CSV; stdout when omitted
--event-file FILE         structured JSONL events
--event-dir DIR           event directory
--event-name NAME         event filename
--log-file FILE           operational text log; alias --application-log-file
--no-event-log            disable events
--no-logging              disable operational logging
```

Do not give `--log-file` a `.jsonl`/`.ndjson` suffix; the CLI rejects it.

## Multi-cache benchmark

```bash
python3 tools/polygraph.py benchmark-caches DOMAIN \
  --cache-configs A.json B.json C.json \
  --repetitions 5 \
  --max-depth 10 \
  --direction both \
  --overwrite
```

Defaults: repetitions `5`, max depth `10`, direction `both`, cooldown `0 ms`, Redis batch `10000`. Optional `--start-uuids-file`, `--thread-count`, `--neighbor-batch-size`, `--skip-redis-load`, `--redis-no-reset`, `--cache-root`, and `--planning-db` are available.

To measure the actual HTTP server path, add `--transport http`:

```bash
python3 tools/polygraph.py benchmark-caches DOMAIN \
  --transport http \
  --cache-configs config/cache/*.json \
  --start-uuids-file data/ldbc-snb/sf10_0/schema-based/start_uuids.txt \
  --repetitions 5 \
  --output-dir measurements/sf10-all-caches \
  --overwrite \
  -- \
  --no-predecessors \
  --neo4j-max-concurrent-requests 16 \
  --arango-max-concurrent-requests 16
```


If the cache directories are already generated, pass their plans directly and no planning/artifact generation is performed:

```bash
python3 tools/polygraph.py benchmark-caches DOMAIN \
  --transport http \
  --cache-plans \
    data/.../caches/cache-a/cache-plan.json \
    data/.../caches/cache-b/cache-plan.json \
  --start-uuids-file starts.txt \
  --repetitions 5 \
  --output-dir measurements/existing-caches \
  --overwrite \
  -- \
  --no-predecessors \
  --neo4j-max-concurrent-requests 16 \
  --arango-max-concurrent-requests 16
```

`--cache-plans` reuses `residency.bin`, MMAP, Redis source artifacts, and GPU selection metadata as-is. Redis itself is loaded by default because it is external runtime state; use `--skip-redis-load` only if the exact plan is already materialized there.

HTTP-specific options are `--http-server-binary`, `--http-server-config`, `--http-host`, `--http-port` (default `8080`), `--http-startup-timeout`, `--http-request-timeout`, and `--http-response-mode summary|full`. The default `summary` response avoids making a large result JSON transfer dominate BFS timing; `full` benchmarks the complete API response.

## `analyze-logs`

Use this for finished structured event files:

```bash
python3 tools/polygraph.py analyze-logs measurements/RUN/events \
  --out-dir measurements/RUN/reconstructed
```

Do not point it at `.log` files.

## `live-logs`

```bash
python3 tools/polygraph.py live-logs --watch-dir measurements/RUN/events
```

The live server can be started before a measurement and can attach to event files as they appear.

## Storage/service helpers

`services` manages the default local storage stack. `instance` manages named Compose instances, including `up`, `start`, `stop`, `restart`, `down`, `status`, `logs`, `load`, `benchmark`, `destroy`, `project`, and `options` actions. Use `--help` on the specific action because instance configuration contains environment-specific ports, credentials, and scale-factor settings.
