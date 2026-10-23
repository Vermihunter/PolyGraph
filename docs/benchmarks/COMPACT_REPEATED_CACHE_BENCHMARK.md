# Compact repeated cache benchmark

This variant keeps the final benchmark tree flat while using one warmup round and
`N` measured rounds for every cache configuration.

## Run

```bash
python3 tools/measurement/benchmark_compact_repeated.py \
  --domain data/ldbc-snb/sf1_0/schema-based/domain.json \
  --cache-configs \
    config/cache/benchmark/sf1/04-gpu30-redis20-mmap20.json \
    config/cache/benchmark/sf1/05-gpu20-redis15-mmap15.json \
  --cache-root data/ldbc-snb/sf1_0/schema-based/caches \
  --request-plan config/benchmarks/sf1-cache-http-requests.example.json \
  --repetitions 10 \
  --output-dir measurements/sf1-cache-N10 \
  --overwrite
```

Add `--restart-arango-before-each-cache` if every cache configuration should get
an independent Arango restart + edge/index-cache prefill before PolyGraph starts.

The source request plan is treated as a set of measured request templates. Its
old warmup entries are replaced. The generated request order is:

```text
warmup root 1        measure=false
warmup root 2        measure=false
...
round 001 request 1  measure=true
round 001 request 2  measure=true
...
round 002 request 1  measure=true
...
round N request M    measure=true
```

The final tree is intentionally compact:

```text
measurements/sf1-cache-N10/
├── _benchmark/
│   ├── request-plan.json
│   └── request-plan.metadata.json
├── 04-gpu30-redis20-mmap20/
│   ├── measurements.csv
│   ├── summary.json
│   ├── events/
│   │   ├── server.events.jsonl
│   │   └── requests/
│   │       ├── index.json
│   │       └── *.events.jsonl
│   ├── logs/
│   ├── outputs/
│   └── server/
├── 05-gpu20-redis15-mmap15/
│   └── ...
├── comparison.csv
├── comparison.json
├── comparison.md
├── benchmark.json
└── benchmark.timeline.jsonl
```

No `01-cache/benchmark/01-cache/benchmark/...` nesting remains after a successful
run.

## Reconstruct all cache runs + Prometheus metrics + global comparison

The existing reconstruction tooling already supports one dashboard per run type,
Prometheus export/visualization, and one comparison page.

```bash
tools/measurement/reconstruct_compact_benchmark.sh \
  measurements/sf1-cache-N10 \
  config/measurement/prometheus-paper.example.json
```

Equivalent explicit command:

```bash
python3 tools/measurement/log_reconstruction.py \
  measurements/sf1-cache-N10 \
  --out-dir measurements/sf1-cache-N10/reconstruction \
  --split-by-run-type \
  --with-compare \
  --metrics-config config/measurement/prometheus-paper.example.json \
  --refresh-metrics \
  --inline-plotly
```

Outputs include:

```text
reconstruction/index.html
reconstruction/compare.html
reconstruction/runs/04-gpu30-redis20-mmap20/dashboard.html
reconstruction/runs/05-gpu20-redis15-mmap15/dashboard.html
reconstruction/runs/<cache>/metrics/...
```

`server.events.jsonl` is available but disabled by default in the comparison;
per-request `.events.jsonl` files are the comparison samples.

## Reconstruct one cache only

```bash
RUN=measurements/sf1-cache-N10/04-gpu30-redis20-mmap20
python3 tools/measurement/log_reconstruction.py \
  "$RUN/events" \
  --out-dir measurements/sf1-cache-N10/reconstruction/04-gpu30-redis20-mmap20 \
  --with-compare \
  --metrics-config config/measurement/prometheus-paper.example.json \
  --refresh-metrics \
  --inline-plotly
```

## Comparison only

```bash
python3 tools/measurement/log_reconstruction.py \
  measurements/sf1-cache-N10 \
  --out-dir measurements/sf1-cache-N10/comparison-only \
  --compare-only \
  --inline-plotly
```
