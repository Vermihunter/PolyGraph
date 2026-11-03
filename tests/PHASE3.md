# Phase 3 — production BFS executor contracts

Phase 3 is complete for database-free execution of the production `BfsExecutor`.
No production code or Phase 2 oracle code was changed. No Phase 4 integration,
real database connection, GPU execution, or commit was performed.

## Changed files

| File | Purpose |
| --- | --- |
| `support/ScriptedGraphStore.hpp` | Existing untracked interface retained; immutable topology, resident source ownership, scripted hooks and bounded gates. |
| `support/ScriptedGraphStore.cpp` | Thread-safe one-hop rows, concurrent candidate batches, complete bidirectional pull, call snapshots, failure injection and buffer poisoning. |
| `scripted_graph_store_test.cpp` | Existing seven untracked tests retained and registered; added concurrent-call and empty-failure regressions. |
| `generate_executor_fixtures.py` | Build-time bridge importing the unchanged `tests.reference.bfs` oracle. |
| `support/ExecutorContract.hpp` | Encoded-ID translation, real registry/catalog/executor setup, exact-result and layer validators. |
| `bfs_contract_test.cpp` | Oracle matrix and explicit policy/filter/unknown-source tests. |
| `bfs_concurrency_test.cpp` | Deterministic schedules and exception propagation tests. |
| `executor_test_labels.cmake`, `CMakeLists.txt` | CTest/CLion discovery, labels, timeouts and regeneration dependencies. |
| `PHASE3.md` | This coverage and verification record. |

The store copies its input topology before publication. Resident sets, vertex
lookup and configuration are immutable; a mutex protects call history and
active/completed counts. Hooks run outside that mutex. Each candidate worker owns
a reusable buffer; its contents are overwritten immediately after consumption.
Workers join before captured state is destroyed or exceptions are rethrown.
Pull callbacks are synchronous. This fake models traversal, not database query
visitors, edge-reference payloads, or backend-native BFS.

## Contracts checked

Nine topologies (chain, diamond, cycle, disconnected graph, self-loops, duplicate
edges, mixed types, switching fan-out/fan-in, isolated root) × two directions ×
four projections × four limits (0, 1, 2, unlimited) give **288 oracle cases**.
Each runs on one owner and alternating Neo4j/Arango owner partitions, with/without
predecessors, through six policy configurations, using both `run` and
`run_streaming`: **13,824 executor invocations** in the main matrix.

The six configurations are Classic, adaptive with no pull store, adaptive with
partial coverage, adaptive with stale coverage, complete sparse pull, and
complete bitmap pull. Directed requests deliberately fall back to push because
the scripted pull store advertises bidirectional support only. Explicit tests
force filtered pull (including rejected relationships/destination types), and
prove a push → pull → push sequence under both sparse and bitmap membership.
Classic is also tested with a complete pull-capable store present.

Expected distances and projected adjacency are calculated by Phase 2 Python
`project`/`bfs`, using logical typed IDs independent of production encoded IDs.
The C++ code translates identity only; it does not compute expected reachability
or distances. The oracle emits its complete projected adjacency so any valid
shortest-path predecessor is accepted, regardless of scheduling/tie order.
Assertions check:

- Exact vertex-to-distance maps, including no missing or extra vertices.
- Exactly one parent per reachable non-root when requested, an allowed projected
  edge to that child, and parent distance exactly one less; no parents otherwise.
- Complete nonempty layers in distance order, no repeated vertices, retained
  vertex types, matching tree edges, and callbacks on the caller thread after
  partition work has finished.
- Every pushed frontier vertex routed to its encoded owner at its oracle depth;
  correct row/candidate API, query metadata, and no expansion at depth zero.

Concurrency tests force two owner partitions into the same expansion, hold one
while its peer finishes, and prohibit advancing depth before all work completes.
They force four workers to attempt duplicate discoveries, then verify the entire
oracle result after buffer poisoning/reuse. They test exceptions before work,
after a partial candidate callback, in candidate worker hooks, in pull, and in
user layer callbacks. A blocked peer must finish before a backend exception
returns; a subsequent query must succeed with fresh state. Classic, adaptive
fallback, and the actual adaptive dense-state push path are all covered.
The store itself is tested with four simultaneous callers.

All gate waits are bounded at five seconds. CTest applies 20-second executor and
30-second store-test timeouts. These bounds protect tests from hangs; they do
not establish production cancellation of a backend that never returns.

## Verification results (2026-11-03)

| Verification | Exact result |
| --- | --- |
| Full `cpu-tests` build, including affected libraries, CLI and existing tests | Passed |
| New correctness tests | **323/323 passed**, zero skipped (292 executor correctness, 22 concurrency/failure, 9 store tests) |
| Concurrency repeat | **25 tests × 50 = 1,250 passes**, zero failures; includes three threaded store tests |
| Existing Phase 2 Python oracle unit tests | **88/88 passed** |
| GCC 11 AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer | **323/323 passed**, no sanitizer reports |
| Clang 21 ThreadSanitizer | **323/323 passed**, no sanitizer reports |
| Entire existing and new CTest suite | **467/468 passed**; only preserved CLI duplicate-alias failure |
| Discovery properties | 323 correctness and 25 concurrency labels; all timeouts verified through CTest JSON |
| Oracle-fixture regeneration | Touching generator schedules JSON regeneration and relinking/discovery of both executor targets; fixture checksum unchanged |
| Whitespace validation | `git diff --check` passed |

The full-suite failure is
`unit.CommandLineTest.RejectsUnknownAndDuplicateOptions`: registering `--verbose`
a second time does not throw `std::invalid_argument`. This was explicitly left
unchanged. An initial sandbox run also failed the existing
`RedisGraphStoreTest.UsesOneMgetAndParsesFragmentedArrayResponse` at socket creation;
it passed outside the socket-restricted sandbox using its local fake server.
No real Redis/database service was used.

No production BFS defect was found. Review caught gaps in early test coverage
(filtered cases never entered pull, adaptive concurrency fell back to Classic,
and regenerated case names did not refresh discovery); all were corrected and
reverified. One new fake-store edge case was first demonstrated by a failing
regression: `fail_after_batches=0` with multiple workers and no candidates did
not throw because zero workers were launched. Failure injection now happens
before worker dispatch; that regression passes. The original seven store tests
remain present.

GCC 11 TSan initially could not start with this host's ASLR; process-local
`setarch x86_64 -R` resolved startup. It then produced false double-lock/race
reports even for a standalone standard-library mutex/condition-variable probe.
The installed runtime lacks the `pthread_cond_clockwait` interceptor used by the
C++ headers; Clang 21 has it. The final TSan result above uses Clang 21 without
suppressions. LeakSanitizer ran outside sandbox ptrace with leak detection
enabled. These are environment workarounds, not disabled checks.

## Reproduce

Normal build and verification:

```bash
cmake --preset cpu-tests
cmake --build --preset cpu-tests
ctest --test-dir build/cpu-tests -L correctness -j 4 --output-on-failure
ctest --test-dir build/cpu-tests -L concurrency --repeat until-fail:50 -j 4 --output-on-failure
python3 -m unittest discover -s tests/reference -t . -p 'test_*.py' -v
ctest --test-dir build/cpu-tests -j 4 --output-on-failure
```

These new targets also work with the database-free `cpu-core` preset. Use the
existing local dependency cache or installed dependencies when offline.

Sanitizer configurations used all core/fake/test code instrumented, with checked
libstdc++ assertions. A separate build per sanitizer is required:

```bash
cmake --preset cpu-core -B build/phase3-asan \
  -DPOLYGRAPH_ENABLE_CLANG_FORMAT=OFF -DPOLYGRAPH_ENABLE_CLANG_TIDY=OFF \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -D_GLIBCXX_ASSERTIONS' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined -no-pie'
cmake --build build/phase3-asan -j 2 --target \
  polygraph_scripted_graph_store_test polygraph_bfs_contract_test polygraph_bfs_concurrency_test
ctest --test-dir build/phase3-asan -L correctness -j 4 --output-on-failure

cmake --preset cpu-core -B build/phase3-tsan-clang -DCMAKE_CXX_COMPILER=clang++-21 \
  -DPOLYGRAPH_ENABLE_CLANG_FORMAT=OFF -DPOLYGRAPH_ENABLE_CLANG_TIDY=OFF \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer -fno-pie -D_GLIBCXX_ASSERTIONS -O1' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread -no-pie'
cmake --build build/phase3-tsan-clang -j 2 --target \
  polygraph_scripted_graph_store_test polygraph_bfs_contract_test polygraph_bfs_concurrency_test
setarch x86_64 -R ctest --test-dir build/phase3-tsan-clang -L correctness -j 4 --output-on-failure
```

On this host LeakSanitizer requires running outside the sandbox's ptrace wrapper.
Use the same `setarch` prefix for TSan build-time test discovery if startup fails.
Logs for this implementation run are retained in ignored
`build/phase3-verification/`.

## Scope and remaining limitations

No new test is disabled or skipped. Real adapter behavior, native BFS pushdown,
GPU kernels, asynchronous pull-consumer implementations, real LDBC execution
through production stores, distributed processes, performance and arbitrary
large-graph schedules remain outside this phase. The independent oracle was
previously validated against SF0.1; this phase uses synthetic projections of its
model and does not imply a new production-SF0.1 comparison. The tested pull fake
has a complete universe, while partial/stale/unsupported-directed stores must
fall back to push. Tests establish the stated contracts for these fixtures and
forced schedules, not a formal proof over all graphs/interleavings. The existing
CLI failure remains unresolved as requested.
