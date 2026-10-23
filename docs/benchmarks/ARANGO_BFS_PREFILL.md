# ArangoDB BFS edge/index-cache prefill benchmark

This profile is for **steady-state BFS measurements**. It intentionally warms
ArangoDB's BFS-relevant edge/index cache before measured PolyGraph work begins.

It does not delete volumes and it does not put the full graph into a second
PolyGraph in-memory representation.

## Memory/cache profile

Default profile:

```text
Arango edge/index cache             6 GiB
cache high-water multiplier         0.90
maximum single cached value        64 MiB
edge-cache compression threshold   100 B

RocksDB block cache                  3 GiB
RocksDB write buffers              512 MiB
strict block-cache limit             yes

index-cache fill on startup          yes
concurrent index fill tasks            8
```

The edge/index cache and RocksDB block cache are separate memory pools.

The values are **capacities**, not preallocated RSS. `docker stats` therefore
does not need to show 9.5 GiB after startup.

## One configuration

Restart ArangoDB and wait for startup/index-cache activity to settle:

```bash
tools/measurement/restart_arango_bfs_warm.sh \
  polygraph-sf1-0-schema-based \
  measurements/arango-prefill/prefill.json
```

Then use the normal diagnostic benchmark with a single complete representative
warmup round:

```bash
python3 tools/measurement/benchmark_steady_arango.py \
  --domain data/ldbc-snb/sf1_0/schema-based/domain.json \
  --cache-configs config/cache/benchmark/sf1/04-gpu30-redis20-mmap20.json \
  --cache-root data/ldbc-snb/sf1_0/schema-based/caches \
  --request-plan config/benchmarks/sf1-cache-http-requests.example.json \
  --warmup-rounds 1 \
  --output-dir measurements/04-warm-arango \
  --overwrite
```

## Recommended fair 04-vs-05 comparison

Use the wrapper. It restarts and prefills ArangoDB **before every placement**,
then runs one unmeasured representative-root warmup round and the measured
requests.

```bash
python3 tools/measurement/run_warm_arango_benchmarks.py \
  --domain data/ldbc-snb/sf1_0/schema-based/domain.json \
  --cache-configs \
    config/cache/benchmark/sf1/04-gpu30-redis20-mmap20.json \
    config/cache/benchmark/sf1/05-gpu20-redis15-mmap15.json \
  --cache-root data/ldbc-snb/sf1_0/schema-based/caches \
  --request-plan config/benchmarks/sf1-cache-http-requests.example.json \
  --output-dir measurements/sf1-0_benchmarks/warm-arango-04-vs-05 \
  --overwrite
```

This avoids the unfair sequence:

```text
04 changes Arango cache state
        ↓
05 inherits that state
```

Instead:

```text
restart + index-cache prefill → 04 → one PolyGraph warmup round → measure
restart + index-cache prefill → 05 → one PolyGraph warmup round → measure
```

## What to verify before trusting measurements

The restart script prints the exact ArangoDB command. It must contain:

```text
--cache.size=6442450944
--cache.high-water-multiplier=0.90
--cache.max-cache-value-size=67108864
--cache.min-value-size-for-edge-compression=100
--rocksdb.auto-fill-index-caches-on-startup=true
--rocksdb.max-concurrent-index-fill-tasks=8
--rocksdb.block-cache-size=3221225472
```

The prefill waiter also refuses to continue if startup auto-fill is not enabled.

## If the BFS is still ~300–400 seconds

Do not increase memory blindly. Compare:

```text
<run>/arango-prefill/prefill.json
<run>/benchmark/.../arango-diagnostics/request-correlation.csv
<run>/benchmark/.../arango-diagnostics/metrics.jsonl
```

Interpretation:

- prefill complete + BFS physical reads low + BFS very slow:
  investigate AQL/index/CPU execution;
- prefill complete + BFS still physically rereads multiple GiB:
  the edge/index prefill is not enough and the RocksDB/Linux working set is
  still cold or exceeds available memory;
- 04 fast but 05 slow after **independent** restart+prefill:
  this is strong evidence that the placement itself changes the authoritative
  backend working set rather than merely inheriting random cache state.

For whole-system memory pressure, keep `vmstat 1` running. For storage
saturation, use `iostat -xz 1`.
