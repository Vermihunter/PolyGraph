# PolyGraph low-memory BFS experiment v4

This version is rebased around the current hot-path files supplied on 2026-09-01.
The Git patch installs only new files. The source rewriter then validates every
hot-file anchor before writing anything.

Implemented experiments:

1. 3 GiB RocksDB block cache + 2 GiB Arango edge/index cache.
2. Arango edge-cache compression for values >= 100 bytes.
3. 256 MiB RocksDB write-buffer budget for the read-heavy benchmark.
4. 50k Arango frontier batches and four request workers by default.
5. Compact candidate rows that omit repeated collection-name prefixes.
6. Experimental one-hop multi-edge traversal to avoid resending the same
   frontier once per edge collection.
7. Independently switchable Arango cursor streaming.
8. Compact summary-mode BFS visited set instead of an external distance hash map.
9. A 50%-top-degree MMAP experiment using the existing PolyGraph MMAP tier.

Apply:

```bash
git apply --check ~/Downloads/polygraph-low-memory-fast-v4-exact-bootstrap.patch
git apply ~/Downloads/polygraph-low-memory-fast-v4-exact-bootstrap.patch
python3 tools/measurement/apply_low_memory_fast_v4.py
git diff --check
cmake --build build -j"$(nproc)" --target polygraph_http_server
```

Restart only ArangoDB, preserving volumes:

```bash
tools/measurement/restart_arango_low_memory_v4.sh polygraph-sf1-0-schema-based
```

Run:

```bash
tools/measurement/run_sf1_low_memory_v4.sh
```

Useful A/B tests:

```bash
POLYGRAPH_ARANGO_MULTI_EDGE_TRAVERSAL=0 tools/measurement/run_sf1_low_memory_v4.sh measurements/lowmem-edge-index

POLYGRAPH_ARANGO_COMPACT_ENDPOINTS=0 tools/measurement/run_sf1_low_memory_v4.sh measurements/lowmem-full-ids

POLYGRAPH_ARANGO_STREAM_CURSOR=1 tools/measurement/run_sf1_low_memory_v4.sh measurements/lowmem-stream

POLYGRAPH_ARANGO_LOW_MEMORY_BATCH_SIZE=25000 tools/measurement/run_sf1_low_memory_v4.sh measurements/lowmem-25k-4

POLYGRAPH_ARANGO_LOW_MEMORY_MAX_CONCURRENCY=8 tools/measurement/run_sf1_low_memory_v4.sh measurements/lowmem-50k-8
```

The multi-edge mode is experimental: it removes repeated frontier serialization,
but Arango native traversal can be slower than direct endpoint-index scans. Keep
it only if your event timings show a win.

V8/Foxx disabling is intentionally not bundled because it changes server
capabilities, not only the BFS/storage hot path. Benchmark that separately after
the changes above are isolated.
