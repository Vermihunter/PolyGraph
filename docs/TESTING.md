# Testing

## Python tooling

Run the tooling tests from the repository root:

```bash
pytest -q tools/graphio tools/measurement
```

Important cache-planner regressions cover:

- canonical `graph.jsonl` remains byte-for-byte unchanged;
- `residency.bin` contains selected cache locations;
- a second cache configuration reuses `cache-plan.sqlite3` without reindexing;
- cache selection can change without stale Redis bits leaking between plans;
- benchmark command construction forwards BFS tuning options.

## C++ tests

Configure a test-enabled preset and use CTest:

```bash
cmake --preset cpu-core
cmake --build --preset cpu-core
ctest --preset cpu-core
```

or a broader test-enabled preset such as `full-cpu`.

## Smoke test

For runtime changes, a useful smoke sequence is:

1. prepare a small LDBC/University graph;
2. load persistent stores;
3. build two cache plans from the same planning DB;
4. materialize Redis for one plan;
5. run BFS with both plans;
6. compare output hashes/vertex counts;
7. ensure event files parse as JSONL and operational logs remain text.
