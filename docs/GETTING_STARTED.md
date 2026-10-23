# Getting started: repository to BFS

This is the recommended end-to-end workflow. The important design rule is that **persistent graph data and cache plans are separate**. Prepare and load the canonical graph first; create as many cache configurations as you want afterwards.

## 1. Clone and install prerequisites

```bash
git clone <your-polygraph-repository-url> polygraph
cd polygraph

python3 -m venv .venv
source .venv/bin/activate
pip install -r tools/requirements.txt
```

For GPU builds, install the RAPIDS/cuGraph and CUDA dependencies expected by your local `gpu-bfs` CMake preset. For the local storage layer, Docker with Compose is required.

## 2. Build the BFS executable

CPU:

```bash
python3 tools/polygraph.py build --preset cpu-bfs
```

GPU/cuGraph:

```bash
python3 tools/polygraph.py build --preset gpu-bfs
```

Equivalent CMake commands are documented in [BUILDING.md](BUILDING.md).

## 3. Obtain LDBC SNB data

### Option A — download a published dataset

Published datasets are available from the LDBC SNB datasets page:

`https://ldbcouncil.org/benchmarks/snb/datasets/`

For a `.tar.zst` archive, for example:

```bash
mkdir -p data/ldbc-snb/sf1_0/raw

tar --use-compress-program=unzstd \
  -xvf ~/Downloads/social_network-sf1-CsvBasic-LongDateFormatter.tar.zst \
  -C data/ldbc-snb/sf1_0/raw
```

The directory passed to PolyGraph must contain the extracted CsvBasic snapshot with `static/` and `dynamic/` content.

### Option B — generate LDBC data locally

PolyGraph can run the LDBC generator through its tooling:

```bash
python3 tools/polygraph.py generate ldbc \
  --sf 1.0 \
  --output-dir data/ldbc-snb/sf1_0/schema-based
```

Use `python3 tools/polygraph.py generate ldbc --help` for generator image, memory, partition, and force options.

## 4. Prepare the immutable canonical graph

Do this **without a cache configuration** in the recommended workflow:

```bash
python3 tools/polygraph.py prepare ldbc \
  --input data/ldbc-snb/sf1_0/raw/social_network-sf1-CsvBasic-LongDateFormatter \
  --sf 1.0 \
  --domain-id ldbc-snb-sf1_0-schema-based \
  --output-dir data/ldbc-snb/sf1_0/schema-based \
  --placement-config data/ldbc-snb/placement.config.json \
  --root-count 100
```

The output contains `domain.json`, the logical schema, backend configuration, roots/start IDs, and `graph.jsonl`. `graph.jsonl` is the canonical graph and is not rewritten when cache strategies change.

## 5. Start the persistent backends

For the repository-managed local services:

```bash
python3 tools/polygraph.py services start
```

or use the named-instance workflow when running multiple scale factors:

```bash
python3 tools/polygraph.py instance up <instance-name>
```

## 6. Load the canonical graph

```bash
python3 tools/polygraph.py load \
  data/ldbc-snb/sf1_0/schema-based/domain.json \
  --reset \
  --batch-size 20000
```

Loading reports progress roughly every five seconds during long stages. `--only-source Neo4J`, `--only-source ArangoDB`, or `--only-source PostgreSQL` can restrict a load. PostgreSQL-specific setup and placement examples are in [POSTGRESQL.md](POSTGRESQL.md).

If Neo4j native IDs are resolved during loading, the domain manifest is updated with the resolved graph/native-ID sidecar as required by the existing loader workflow.

## 7. Build one or more cache plans

A cache plan is independent of the graph and persistent databases:

```bash
python3 tools/polygraph.py cache build \
  data/ldbc-snb/sf1_0/schema-based/domain.json \
  --config \
    config/cache/ldbc-cache.json \
    config/cache/ldbc-cache.sf10.json
```

The first run parses/indexes the graph into a reusable `cache-plan.sqlite3`. Later configurations reuse that SQLite database, reset only cache-selection state, and produce separate directories:

```text
graph.jsonl                 canonical immutable graph
cache-plan.sqlite3          reusable graph/index database
caches/
  ldbc-cache/
    residency.bin
    redis/
    mmap/
    cache-plan.json
  ldbc-cache.sf10/
    residency.bin
    redis/
    mmap/
    cache-plan.json
```

If the selected plan uses Redis, materialize it with:

```bash
python3 tools/polygraph.py cache redis-load \
  data/ldbc-snb/sf1_0/schema-based/domain.json \
  --cache-plan data/ldbc-snb/sf1_0/schema-based/caches/ldbc-cache/cache-plan.json
```

Redis is reset by default so stale keys from another plan cannot masquerade as valid cache residency. Use `--no-reset` only when you deliberately manage the Redis DB yourself.

## 8. Run one BFS

Direct executable example:

```bash
START_UUID="$(head -n 1 data/ldbc-snb/sf1_0/schema-based/start_uuids.txt)"

./build/gpu-bfs/bin/polygraph_bfs \
  --domain data/ldbc-snb/sf1_0/schema-based/domain.json \
  --cache-plan data/ldbc-snb/sf1_0/schema-based/caches/ldbc-cache/cache-plan.json \
  --start-uuid "$START_UUID" \
  --max-depth 10 \
  --direction both \
  --output-file measurements/manual/result.csv \
  --event-file measurements/manual/events.events.jsonl \
  --log-file measurements/manual/polygraph.log
```

Operational logs are text and are also emitted to stdout. Structured execution events are JSONL. Keeping the result in `--output-file` avoids mixing live stdout logs with CSV output.

The wrapper provides better defaults:

```bash
python3 tools/polygraph.py run \
  data/ldbc-snb/sf1_0/schema-based/domain.json \
  --start-uuid "$START_UUID" \
  --repetitions 5 \
  --run-id sf1-smoke \
  -- \
  --cache-plan data/ldbc-snb/sf1_0/schema-based/caches/ldbc-cache/cache-plan.json \
  --max-depth 10 \
  --direction both
```

It creates separate `events/`, `logs/`, and `outputs/` trees under `measurements/sf1-smoke/`.

## 9. Reconstruct and inspect event logs

```bash
python3 tools/polygraph.py analyze-logs \
  measurements/sf1-smoke/events \
  --out-dir measurements/sf1-smoke/reconstructed
```

For live viewing:

```bash
python3 tools/polygraph.py live-logs \
  --watch-dir measurements/sf1-smoke/events
```

The generated dashboard also has **Compare runs**. When the input contains benchmark
configuration directories such as `08-no-cache/events/requests/*.events.jsonl`, the
comparison view groups request/repetition logs by configuration. Pick one primary
metric (for example `full_bfs`) to get one boxplot per configuration, then optionally
show median + IQR backend/event breakdowns. `server.events.jsonl` is available but
disabled by default because it covers the whole server lifetime rather than one BFS
request.

See [OBSERVABILITY.md](OBSERVABILITY.md) and [SCRIPTS.md](SCRIPTS.md) for the event/log distinction and reconstruction options.

## 10. Benchmark cache configurations

```bash
python3 tools/polygraph.py benchmark-caches data/ldbc-snb/sf1_0/schema-based/domain.json \
  --cache-configs \
    config/cache/cache-a.json \
    config/cache/cache-b.json \
    config/cache/cache-c.json \
  --repetitions 10 \
  --max-depth 10 \
  --direction both \
  --overwrite
```

Every cache configuration uses the same immutable graph and reusable SQLite planning DB. The script generates/materializes the plan, starts the benchmark executable with `--cache-plan`, and writes `comparison.csv`, `comparison.json`, and a readable `comparison.md`.
