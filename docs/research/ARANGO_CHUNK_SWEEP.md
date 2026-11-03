# ArangoDB fixed-thread chunk-size sweep

This experiment varies the request chunk used by PolyGraph's current ArangoDB
BFS hot path while keeping Arango's scheduler width fixed.

The runner pins `--server.minimal-threads` and `--server.maximal-threads` to the
same value, defaults PolyGraph's `arangoMaxConcurrentRequests` to the same
value, forces A1 direct edge-index access with the AQL plan cache enabled, and
forces `arangoRequestStrategy=chunked` so `neighborBatchSize` always means the
number of frontier vertices packed into one Arango request.

Default experiment:

```text
Arango scheduler threads:       16
PolyGraph Arango concurrency:   16
chunk sizes:                    1,024  4,096  16,384  65,536  250,000
cache configs:                  04-gpu30-redis20-mmap20
                                05-gpu20-redis15-mmap15
                                08-no-cache
```

Quick single-cache smoke test:

```bash
ARANGO_SWEEP_CACHE_NAMES=04-gpu30-redis20-mmap20 \
  tools/measurement/run_arango_chunk_sweep.sh 1
```

Default sweep:

```bash
tools/measurement/run_arango_chunk_sweep.sh 5
```

For a stronger order-balanced experiment, sweep forward and reverse:

```bash
ARANGO_SWEEP_PASSES=2 tools/measurement/run_arango_chunk_sweep.sh 10
```

Custom chunks:

```bash
ARANGO_SWEEP_THREADS=16 \
ARANGO_SWEEP_CONCURRENCY=16 \
ARANGO_SWEEP_CHUNK_SIZES=2048,8192,32768,131072,250000 \
ARANGO_SWEEP_CACHE_NAMES=04-gpu30-redis20-mmap20,05-gpu20-redis15-mmap15,08-no-cache \
  tools/measurement/run_arango_chunk_sweep.sh 5
```

Results are written under `measurements/sf1-arango-chunk-sweep-*`. The main
summary is `_analysis/summary.md`; machine-readable outputs are
`_analysis/aggregate.csv`, `_analysis/per_run.csv`, and `_analysis/summary.json`.
