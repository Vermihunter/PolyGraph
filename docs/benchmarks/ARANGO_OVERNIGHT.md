# Arango overnight benchmark

This suite collects statistically useful measurements for the practical ArangoDB
BFS access path after the A0 traversal experiments established that full-BFS A0
is outside the useful runtime range for the tested SF1 workload.

## What it measures

The suite compares:

- `a1-edge-index`: direct native `_from` / `_to` edge-index access;
- `a1-edge-index-plan-cache`: the same access path with AQL execution-plan cache enabled.

The default cache configurations are:

- `04-gpu30-redis20-mmap20`;
- `05-gpu20-redis15-mmap15`;
- `08-no-cache`.

Before every cache configuration, the existing ArangoDB container is restarted
without deleting its persistent volumes. One cold-prime BFS is then recorded and
discarded from the warm statistics. Linux host page cache is deliberately not
dropped because doing so would simultaneously perturb MMAP, Neo4j, and
PostgreSQL.

The suite performs four passes:

1. A1, forward cache order;
2. A1 + plan cache, reverse cache order;
3. A1 + plan cache, forward cache order;
4. A1, reverse cache order.

This balances both variant order and cache order. With the default 15 repetitions
per pass, each variant/cache pair gets 30 measured rounds. With the default
three-root request plan that is 90 measured BFS requests per pair.

## Run

If the PolyGraph storage services are already running and healthy:

```bash
ARANGO_OVERNIGHT_SKIP_STORAGE_ACTIVATE=1 \
  tools/measurement/run_arango_overnight_benchmark.sh 15
```

To let the suite activate storage once before the first pass:

```bash
ARANGO_OVERNIGHT_SKIP_STORAGE_ACTIVATE=0 \
  tools/measurement/run_arango_overnight_benchmark.sh 15
```

Useful overrides:

```bash
ARANGO_OVERNIGHT_OUT=measurements/my-arango-run
ARANGO_OVERNIGHT_REQUEST_TIMEOUT_SECONDS=300
ARANGO_OVERNIGHT_COLD_PRIME_TIMEOUT_SECONDS=900
ARANGO_OVERNIGHT_PYTHON=/home/akos/miniconda3/bin/python3
```

## Outputs

The merged analysis is written under `<output>/_analysis/`:

- `summary.md` and `summary.json`;
- `statistics.csv` with aggregate warm statistics;
- `per_root.csv` with one distribution per BFS root;
- `cold_prime.csv` and `cold_prime_statistics.csv`;
- `pairwise.csv` with plan-cache and cache-vs-no-cache speedups;
- `passes.csv` with pass ordering/status;
- `server_bfs_boxplot.png`;
- `plan_cache_speedup.png`.

Raw measurements and a complete log for every pass are preserved as well.
