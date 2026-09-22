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
| `grpc-server` | gRPC frontend |
| `full-cpu` | all CPU applications/tests |
| `docs` | Doxygen target |

The GPU presets in `CMakePresets.json` contain environment-specific CUDA/RAPIDS paths. Adjust `POLYGRAPH_DEPENDENCY_PREFIX` and `CMAKE_CUDA_COMPILER` for another machine.

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

See [STATIC_ANALYSIS.md](STATIC_ANALYSIS.md) and [TESTING.md](TESTING.md).
