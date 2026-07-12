# Formatting and static analysis {#static_analysis}

## clang-format gate

When `CPGDB_ENABLE_CLANG_FORMAT=ON`, CMake creates
`cpgdb_clang_format_check`. Every first-party target depends on that target, so a
formatting violation fails before compilation.

Check formatting:

```bash
cmake --build build/cpu-core --target cpgdb_clang_format_check
```

Apply formatting:

```bash
cmake --build build/cpu-core --target format
```

The vendored `include/utilities/json.hpp` is excluded.

## clang-tidy gate

When `CPGDB_ENABLE_CLANG_TIDY=ON`, CMake sets `CXX_CLANG_TIDY` on each
first-party target. Enabled diagnostics are treated as errors. The configuration
lives in `.clang-tidy`.

Run through a normal build:

```bash
cmake --build --preset cpu-core
```

A compilation database is emitted by every preset and can also be used manually:

```bash
clang-tidy -p build/cpu-core src/algorithms/bfs/BfsExecutor.cpp
```

## CI recommendation

Run at least these jobs:

1. `cpu-core` with formatting, tidy, and tests;
2. `cpu-bfs` with adapters and tests;
3. `gpu-bfs` on a RAPIDS/CUDA runner;
4. `docs` with Doxygen warnings treated as errors;
5. HTTP and gRPC presets on runners with their dependencies installed.
