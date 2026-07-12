# Testing guide {#testing_guide}

## Organization

Tests are under `tests/` and use GoogleTest. Each source file builds as a separate
executable and is registered with `gtest_discover_tests()`. CTest applies the
`unit` label to every discovered case.

The suites cover:

- graph value and JSON serialization;
- expansion filtering and host adjacency behavior;
- backend registry and placement planning;
- distributed and native BFS behavior;
- command-line parsing and error handling;
- insertion item/query ownership and visitor dispatch;
- UUID backend encoding;
- load tracking and projected reservations;
- type-based and replicated placement policies;
- insertion batching, failure behavior, and accounting;
- public-header self-containment.

## Run all tests

```bash
cmake --preset cpu-core
cmake --build --preset cpu-core
ctest --preset cpu-core
```

## Run only unit-labelled tests

```bash
ctest --test-dir build/cpu-core -L unit --output-on-failure
```

## Run one executable or case

```bash
./build/cpu-core/bin/cpgdb_insertion_coordinator_test
./build/cpu-core/bin/cpgdb_bfs_executor_test \
  --gtest_filter='*DepthLimits*'
```

## List tests

```bash
ctest --test-dir build/cpu-core -N
./build/cpu-core/bin/cpgdb_insertion_placement_policy_test --gtest_list_tests
```

## Add a test

1. Add a `tests/<name>_test.cpp` file.
2. Register it with `cpgdb_add_unit_test()` in `tests/CMakeLists.txt`.
3. Link the narrowest library that exposes the unit under test.
4. Prefer parameterized tests for direction, depth, backend, type, and error
   matrices.
5. Test observable behavior rather than private implementation details.
