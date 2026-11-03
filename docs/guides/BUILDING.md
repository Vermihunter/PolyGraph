# Building PolyGraph

PolyGraph uses CMake presets with Ninja.

## Recommended wrapper

```bash
python3 tools/polygraph.py build --preset cpu-bfs
python3 tools/polygraph.py build --preset gpu-bfs
```

## Direct CMake

```bash
cmake --preset cpu-bfs
cmake --build --preset cpu-bfs
```

GPU:

```bash
cmake --preset gpu-bfs
cmake --build --preset gpu-bfs
```

## Presets

| Preset | Purpose |
|---|---|
| `cpu-core` | core library + tests, no backends/BFS |
| `cpu-bfs` | release CPU BFS CLI/benchmark |
| `cpu-bfs-debug` | debug CPU BFS |
| `gpu-bfs` | release CUDA/cuGraph BFS |
| `gpu-bfs-debug` | debug CUDA/cuGraph BFS |
| `http-server` | Drogon server |
| `http-server-observability` | Drogon + metrics/jemalloc |
| `full-cpu` | all CPU applications/tests |
| `docs` | Doxygen target |

GPU presets intentionally omit machine-specific CUDA/RAPIDS paths. Configure with `-DPOLYGRAPH_DEPENDENCY_PREFIX=/path/to/rapids` and, when needed, `-DCMAKE_CUDA_COMPILER=/path/to/nvcc`.

## PostgreSQL client dependency

Backend builds include the PostgreSQL adapter and require the official libpq development package. On Ubuntu:

```bash
sudo apt install libpq-dev
```

CMake discovers it with `find_package(PostgreSQL)` and links `PostgreSQL::PostgreSQL`. The Python loader separately uses psycopg 3 from `tools/requirements.txt`.

## Dependency fetching

By default presets enable `POLYGRAPH_FETCH_DEPENDENCIES`. First configuration therefore needs network access for missing FetchContent dependencies. In an offline build environment, preinstall/provide those dependencies or point CMake at an existing dependency prefix.

## Quality checks

Presets enable compile-command export and can enable clang-format/clang-tidy. Disable them temporarily during diagnosis with CMake cache overrides, but CI/development builds should run the configured checks.

See [STATIC_ANALYSIS.md](../operations/STATIC_ANALYSIS.md) and [TESTING.md](TESTING.md).

## Documentation only (no runtime dependencies)

```bash
cmake --preset docs
cmake --build --preset docs
# build/docs/docs/generated/html/index.html
```

The docs preset sets `POLYGRAPH_DOCS_ONLY=ON` and needs Doxygen, but does not fetch or compile C++ dependencies.

## Distributed GPU research binaries (opt-in)

```bash
cmake --preset gpu-bfs -DPOLYGRAPH_BUILD_DISTRIBUTED_GPU_EXPERIMENTS=ON
cmake --build build/gpu-bfs --target polygraph-mg-cugraph-bfs
```

MPI and CUDA-aware MPI executables are separate, and **are not part of production traversal**. See [the research source README](../../src/gpu/distributed/README.md).
