# PolyGraph tooling

The supported operational path is intentionally split into small scripts.  The
large `tools/polygraph.py` CLI remains the reusable core and backwards-compatible
Swiss-army knife; the scripts below are the normal entry points for experiments.

## Storage lifecycle

```bash
# Normal day-to-day command: switch to SF1. If it has never been initialized,
# prepare/load it automatically; otherwise just reuse its volumes.
python3 tools/workflows/use_sf.py --sf 1

# Low-level switch only (never prepares/reloads data).
python3 tools/storage/activate.py --sf 1

# Destructively reset only SF1.
python3 tools/storage/reset.py --sf 1 --yes

# One-time full cleanup of all discovered PolyGraph storage instances.
python3 tools/storage/reset.py --all --yes
```

Compose project names namespace database volumes (`polygraph-sf1-0-schema-based`,
`polygraph-sf10-0-schema-based`, ...). Therefore switching scale factors is cheap:
only the active containers are stopped; their data stays on disk.

## LDBC preparation

```bash
python3 tools/ldbc_snb/prepare_schema.py --sf 1 --clean
```

This prepares a schema-based distribution with exactly three authoritative
backends: Neo4j, ArangoDB and PostgreSQL. GPU, Redis and mmap are cache layers,
not authoritative placement targets.

## Cache plans

```bash
python3 tools/cache/build_benchmark_plans.py --sf 1 --clean
```

For SF1 this consumes every JSON file under `config/cache/benchmark/sf1/` and
reuses one cache-planning SQLite database across all eight plans.

## Full reinitialization

```bash
python3 tools/workflows/reinitialize_sf.py --sf 1 --purge-all-storage
```

That command resets storage, deletes only the generated schema-based distribution
(raw LDBC data is preserved), prepares the graph, starts the target containers,
loads all three databases, and generates all cache plans.

The workflow is resumable by default. It stores local checkpoints under
`.polygraph/workflows/`. Re-run the **same command** after a failure and completed
stages are skipped. Persistent database loads are checkpointed independently, so
if Neo4j and ArangoDB finish but PostgreSQL fails, the next run loads PostgreSQL
only. To intentionally throw the checkpoint away and rebuild from zero, add
`--restart`:

```bash
python3 tools/workflows/reinitialize_sf.py \
  --sf 1 \
  --purge-all-storage \
  --restart
```

`use_sf.py` uses the same per-backend resume mechanism when it has to initialize
an SF automatically.

`--batch-size` is retry-safe and may be changed between attempts. Neo4j also
automatically splits an individual node/proxy/relationship transaction when it
hits `db.memory.transaction.max`, so the common memory-pool OOM does not require
raising the configured memory ceiling.

PostgreSQL traversal indexes are disabled by default. Add
`--postgres-indexes` only when you explicitly want the indexed comparison.

## Benchmark matrix

```bash
python3 tools/measurement/run_cache_matrix.py \
  --config config/benchmarks/sf1-cache-matrix.json \
  --repetitions 10 \
  --overwrite
```

The matrix activates the requested storage SF, ensures the Redis cache service is
up, validates that all eight cache plans exist, creates one unmeasured warm-up
round, then runs `N` measured rounds for every cache configuration.
