# PolyGraph workflows

`tools/polygraph.py` remains the reusable core CLI, but day-to-day experiment
operations are exposed as small scripts grouped by responsibility. See
`tools/README.md` for the concise command map.

## Canonical LDBC storage model

Schema-based LDBC preparation now uses exactly three authoritative persistent
backends:

- Neo4j
- ArangoDB
- PostgreSQL

GPU, Redis and mmap are acceleration/cache layers and are configured independently
through cache plans.

## Prepare and initialize an SF

For the first use of an SF, or after its storage volumes were deleted:

```bash
python3 tools/workflows/use_sf.py --sf 1
```

`use_sf.py` prepares `data/ldbc-snb/sf1_0/schema-based/` from the corresponding
raw directory when needed, activates the SF1 Compose project, and loads all three
persistent databases when the SF has not been initialized yet. Later switches
reuse the same project-scoped volumes.

Equivalent low-level steps:

```bash
python3 tools/ldbc_snb/prepare_schema.py --sf 1 --clean
python3 tools/storage/activate.py --sf 1
python3 tools/polygraph.py instance 1 schema-based load
```

PostgreSQL traversal indexes are disabled by default. Add `--postgres-indexes`
to the load command only for an explicit indexed experiment.

## Full clean rebuild

```bash
python3 tools/workflows/reinitialize_sf.py --sf 1 --purge-all-storage
```

This deletes discovered PolyGraph storage containers/volumes, deletes the
generated schema-based distribution (raw CSVs are preserved), reconverts LDBC,
loads Neo4j + ArangoDB + PostgreSQL, and builds every cache plan under
`config/cache/benchmark/sf1/`.

### Resume after failure

Full rebuilds are checkpointed under `.polygraph/workflows/`. The checkpoint is
outside the generated dataset directory, so `prepare_schema.py --clean` cannot
delete it. Re-running the same command resumes at the last incomplete stage.
Database loading is finer grained: Neo4j, ArangoDB, and PostgreSQL each get their
own checkpoint. For example, if PostgreSQL fails after the other two complete,
the retry prints those two as skipped and starts directly with PostgreSQL.

```bash
# first attempt (or retry -- the command is identical)
python3 tools/workflows/reinitialize_sf.py --sf 1 --purge-all-storage

# explicitly discard progress and perform a genuinely fresh run
python3 tools/workflows/reinitialize_sf.py --sf 1 --purge-all-storage --restart
```

Changing output-defining options (for example `--root-count` or
`--postgres-indexes`) while an incomplete checkpoint exists is rejected instead
of silently mixing incompatible runs; use `--restart` for that case.

Loader tuning options such as `--batch-size` are intentionally *not* part of the
checkpoint identity. If one backend fails under memory pressure, you can retry
the same workflow with a smaller batch and already completed backends remain
skipped. Neo4j additionally adapts automatically: a row transaction that hits
`db.memory.transaction.max` is rolled back and retried as progressively smaller
transactions instead of aborting the whole load.

## Switch scale factors

```bash
python3 tools/workflows/use_sf.py --sf 10
python3 tools/workflows/use_sf.py --sf 1
```

`1`, `1.0`, `sf1`, and `sf1_0` all resolve to `sf1_0`. New Compose projects are
synthesized lazily, so `storage_layer/environments.json` only needs entries when
an SF requires overrides. Activating another SF stops the previous storage
containers but preserves its volumes.

## Cache benchmark matrix

Build/rebuild the eight SF1 cache plans:

```bash
python3 tools/cache/build_benchmark_plans.py --sf 1 --clean
```

Run all eight configurations for `N=10` measured rounds (after one unmeasured
warm-up round):

```bash
python3 tools/measurement/run_cache_matrix.py \
  --config config/benchmarks/sf1-cache-matrix.json \
  --repetitions 10 \
  --overwrite
```

The short wrapper is equivalent:

```bash
tools/measurement/benchmark.sh 10
```
