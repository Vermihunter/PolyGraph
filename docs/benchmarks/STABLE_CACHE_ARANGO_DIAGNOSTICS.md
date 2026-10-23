# Stable cache benchmarking and ArangoDB diagnostics

This experiment driver addresses three separate benchmark problems:

1. **Slow benchmark startup despite existing caches**
   - use `--cache-plans`, never `--cache-configs`;
   - this skips `plan_graph_caches(...)` and therefore skips the reusable
     `cache-plan.sqlite3` planning phase entirely;
   - Redis is still materialized by default because it is external process state.

2. **Uncontrolled warm/cold state**
   - the driver prepends complete representative-root warmup rounds;
   - every warmup request is `measure=false`;
   - old `atMs` values are removed in serial mode, so there is no artificial
     schedule-delay chain;
   - two rounds are the default, allowing the same root to be compared between
     warmup rounds.

3. **ArangoDB looks random**
   - a watcher samples the actual `arangod` host PID every second;
   - it records CPU, RSS, swap, physical block reads/writes, Prometheus metrics,
     exact Docker command/image, Arango version, effective options and every
     edge collection's native edge index;
   - after the run it correlates physical Arango I/O/CPU/RSS with each BFS
     request interval.

## Recommended SF1 command

```bash
python3 tools/measurement/benchmark_steady_arango.py \
  --domain data/ldbc-snb/sf1_0/schema-based/domain.json \
  --cache-configs \
    config/cache/benchmark/sf1/04-gpu30-redis20-mmap20.json \
    config/cache/benchmark/sf1/05-gpu20-redis15-mmap15.json \
  --cache-root data/ldbc-snb/sf1_0/schema-based/caches \
  --request-plan config/benchmarks/sf1-cache-http-requests.example.json \
  --warmup-rounds 2 \
  --output-dir measurements/sf1-0_benchmarks/steady-arango-diagnostic \
  --overwrite
```

`--cache-configs` above is only a convenience lookup. A config named
`04-gpu30-redis20-mmap20.json` is resolved to:

```text
data/ldbc-snb/sf1_0/schema-based/caches/04-gpu30-redis20-mmap20/cache-plan.json
```

The cache planner is **not invoked**.

You can also pass exact plans directly.

## What is produced

For each cache configuration:

```text
NN-cache-name/
├── benchmark/
│   └── cache-name/
│       ├── measurements.csv
│       ├── events/
│       └── ...
└── arango-diagnostics/
    ├── docker-inspect.json
    ├── arangod-version.txt
    ├── effective-options.json
    ├── edge-indexes.json
    ├── process.csv
    ├── metrics.jsonl
    ├── summary.json
    ├── summary.md
    ├── warmup-analysis.json
    ├── warmup-analysis.md
    ├── request-correlation.csv
    ├── request-correlation.json
    └── request-correlation.md
```

## What to look for

### Cold storage / cache warming

A request is slow and `arango_physical_read_bytes` is large. The next warmup
round for the same root becomes fast and its physical-read delta collapses.

### Cache-capacity thrashing

The same representative root remains slow in warmup round 2 and Arango performs
large physical reads again.

### CPU/AQL/index bottleneck

A request is slow, physical block reads are small, but Arango CPU is high.

Then inspect:

- `edge-indexes.json`;
- `effective-options.json`;
- event JSONL, especially `arango.bfs_http_post` and
  `arango.stream.edge_index_query`;
- `/_api/explain` for a representative generated AQL query.

### Memory pressure / swapping

Also run:

```bash
vmstat 1
```

Non-zero `si`/`so` during a slow run means system swapping is contaminating the
benchmark.

### SSD saturation

Also run:

```bash
iostat -xz 1
```

High `%util` and high `await` during the slow BFS points to storage saturation.

For paper measurements, keep warmup requests unmeasured and report cold-start
latency separately from steady-state latency.
