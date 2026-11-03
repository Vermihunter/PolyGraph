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

The CPU test preset builds the core, persistent/cache adapters, CLI, and every
existing CPU GoogleTest executable. It also registers the dependency-free Python
CSV comparison regressions with CTest. It requires libcurl, libpq, Python 3.10+, and
GoogleTest (installed or fetched). No database service or LDBC dataset is needed.

```bash
cmake --preset cpu-tests
cmake --build --preset cpu-tests
ctest --preset cpu-tests --show-only
ctest --preset cpu-tests --output-junit ctest-results.xml
```

`cpu-tests` disables the build's in-place formatter and clang-tidy so running the
suite does not reformat the checkout or require machine-specific clang tools.
Use the usual development presets for those checks. Each registered test has a
30-second execution timeout; the separate discovery timeout is also 30 seconds.
The `cpu-tests` and `cpu-core` test presets fail if no tests are found. Keep failed
tests enabled: CTest reports their assertions and returns a nonzero exit code.
The JUnit report above is written to `build/cpu-tests/ctest-results.xml`.

The Redis adapter test runs its own ephemeral loopback server; it needs permission
to bind a local socket, but does not need Redis installed or running.

For a smaller build without adapters or the CLI:

```bash
cmake --preset cpu-core
cmake --build --preset cpu-core
ctest --preset cpu-core
```

The core preset omits the Redis, CLI, and public-runtime-header executables.

Run the existing Python unit/tooling suite separately (including tiny synthetic
CSV conversion fixtures, not live LDBC integration):

```bash
python3 -m pytest -q tests tools/graphio tools/measurement tools/ldbc_snb \
  tools/workflows tools/test_polygraph_gui.py tools/test_polygraph_instances.py
```

Some Python tooling tests also start local fake HTTP servers. Save CTest output
and JUnit results alongside the Python summary when reporting failures. Do not
treat discovery alone, a skipped suite, or an empty result set as a passing run.

## Independent raw-CSV oracle

The standard-library-only oracle under [`tests/reference`](../tests/reference/README.md)
has its own loader, typed raw-ID graph, deterministic BFS, strict comparator,
dataset lock and explicit SF0.1 roots. Production loading/ID/routing code is
prohibited. Its small-graph, corruption and import-isolation tests run as
`python.reference_oracle` in CTest; they do not require SF0.1 or a database.

```bash
python3 -m unittest discover -s tests/reference -t . -p 'test_*.py' -v
```

The real SF0.1 run is an explicit separate command documented in the oracle
README. Its generated CSVs and report belong under `build/`; the checksum lock
and fixed query manifest belong in Git. No database integration is performed.

## Smoke test

For runtime changes, a useful smoke sequence is:

1. prepare a small LDBC/University graph;
2. load persistent stores;
3. build two cache plans from the same planning DB;
4. materialize Redis for one plan;
5. run BFS with both plans;
6. compare output hashes/vertex counts;
7. ensure event files parse as JSONL and operational logs remain text.
