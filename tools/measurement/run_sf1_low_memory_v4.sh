#!/usr/bin/env bash
set -euo pipefail

OUTPUT_DIR="${1:-measurements/sf1-0_benchmarks/cache-comparison-low-memory-v4}"

export POLYGRAPH_ARANGO_LOW_MEMORY="${POLYGRAPH_ARANGO_LOW_MEMORY:-1}"
export POLYGRAPH_ARANGO_LOW_MEMORY_BATCH_SIZE="${POLYGRAPH_ARANGO_LOW_MEMORY_BATCH_SIZE:-50000}"
export POLYGRAPH_ARANGO_LOW_MEMORY_MAX_CONCURRENCY="${POLYGRAPH_ARANGO_LOW_MEMORY_MAX_CONCURRENCY:-4}"
export POLYGRAPH_ARANGO_COMPACT_ENDPOINTS="${POLYGRAPH_ARANGO_COMPACT_ENDPOINTS:-1}"
export POLYGRAPH_ARANGO_MULTI_EDGE_TRAVERSAL="${POLYGRAPH_ARANGO_MULTI_EDGE_TRAVERSAL:-1}"
export POLYGRAPH_ARANGO_STREAM_CURSOR="${POLYGRAPH_ARANGO_STREAM_CURSOR:-0}"

python3 tools/polygraph.py benchmark-caches   data/ldbc-snb/sf1_0/schema-based/domain.json   --transport http   --cache-configs config/cache/benchmark/sf1/*.json   --http-request-plan config/benchmarks/sf1-cache-http-requests.low-memory-v4.json   --cache-root data/ldbc-snb/sf1_0/schema-based/caches   --planning-db data/ldbc-snb/sf1_0/schema-based/cache-plan.sqlite3   --output-dir "$OUTPUT_DIR"   --overwrite
