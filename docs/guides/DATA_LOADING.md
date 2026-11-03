# Data loading

## Persistent data

Use:

```bash
python3 tools/polygraph.py load DOMAIN --reset --batch-size 20000
```

`--reset` is appropriate when rebuilding a benchmark dataset from scratch. Without it, loaders preserve existing backend state according to their backend-specific behavior.

The loader emits start/completion and throughput progress messages at least every long-running ~5 second interval so SF10/SF30/SF100 work is visible.

## Backend isolation

```bash
python3 tools/polygraph.py load DOMAIN --only-source Neo4J
python3 tools/polygraph.py load DOMAIN --only-source ArangoDB
python3 tools/polygraph.py load DOMAIN --only-source PostgreSQL
```

`--only-source` may be repeated.

## PostgreSQL data

PostgreSQL uses psycopg 3 COPY-based bulk loading. Tables are created if needed, traversal indexes are removed during a fresh bulk load and rebuilt afterwards, and `ANALYZE` is run before the database is used for BFS. Cross-backend endpoint proxies and edge replication follow the same placement rules as the other persistent stores.

## GPU data

Authoritative GPU storage and GPU caching are separate. Vertices whose original placement source is `GPU` are materialized during preparation into reusable dense projections under `gpu-storage/gpu/gpuN/`. A separate GPU distribution configuration can partition those authoritative vertices across several physical GPUs:

```bash
python3 tools/polygraph.py prepare ldbc ... \
  --gpu-distribution-config config/gpu/ldbc-gpu-distribution.example.json
```

If no GPU distribution configuration is supplied, all authoritative GPU-owned data is assigned to GPU 0. The generated `ownership.bin` maps stable canonical IDs to physical `GPU:N` devices, while `gpu-storage-plan.json` describes the reusable projections.

For an already prepared domain the base GPU storage can be rebuilt/rebalanced without reconverting the raw LDBC data:

```bash
python3 tools/polygraph.py gpu-storage build DOMAIN \
  --config config/gpu/ldbc-gpu-distribution.example.json
```

At BFS/server startup the runtime loads the authoritative projection from these binary artifacts. If a selected cache plan also contains a GPU cache projection for the same device, the two projections are loaded independently and merged before cuGraph construction. Cache selection never chooses authoritative GPU-owned vertices as GPU/Redis/MMAP cache residents.

Newly prepared domains therefore do not need a `graph.jsonl` scan to reconstruct authoritative GPU storage at runtime. The scan remains only as a compatibility fallback for older domains without a GPU storage plan.

## Redis data

Redis is materialized only from the selected plan's `redis/index.bin` and `redis/neighbors.bin`. Use the safe default reset unless intentionally sharing the Redis DB:

```bash
python3 tools/polygraph.py cache redis-load DOMAIN --cache-plan CACHE/cache-plan.json
```
