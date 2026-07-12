# Build guide {#build_guide}

## Required CPU dependencies

- CMake 3.26 or newer
- Ninja
- a C++23 compiler (GCC 13+, Clang 17+, or a compatible MSVC release)
- Boost headers
- libcurl when database adapters are enabled
- clang-format and clang-tidy when their gates are enabled
- GoogleTest when tests are enabled, unless CMake is allowed to fetch it

## Optional dependencies

| Component | Dependencies |
|---|---|
| GPU | CUDA Toolkit, RAFT, RMM, cuGraph |
| HTTP | Drogon and JsonCpp |
| gRPC | Protobuf, protoc, gRPC, grpc_cpp_plugin |
| Documentation | Doxygen and Graphviz |

## Primary presets

```bash
cmake --list-presets
```

- `cpu-core`: debug core libraries and unit tests, without database adapters.
- `cpu-bfs`: release BFS CLI, Neo4j/ArangoDB adapters, and tests.
- `cpu-bfs-debug`: debug BFS CLI and backend adapters.
- `gpu-bfs`: release BFS CLI with CUDA/cuGraph support.
- `http-server`: CPU HTTP server with Drogon.
- `grpc-server`: CPU gRPC streaming server.
- `full-cpu`: BFS CLI plus both server frontends.
- `docs`: configure only the Doxygen documentation target.

Every configure preset has a build preset with the same name. Test-capable
presets also have matching test presets.

## Static-analysis tools

The default build requires clang-format and clang-tidy. When several LLVM
versions are installed, pass exact paths:

```bash
cmake --preset cpu-core \
  -DCPGDB_CLANG_FORMAT_EXECUTABLE=/usr/bin/clang-format-21 \
  -DCPGDB_CLANG_TIDY_EXECUTABLE=/usr/bin/clang-tidy-21
```

For a dependency investigation only, the gates may be disabled explicitly:

```bash
cmake --preset cpu-core \
  -DCPGDB_ENABLE_CLANG_FORMAT=OFF \
  -DCPGDB_ENABLE_CLANG_TIDY=OFF
```

## Offline builds

Presets do not fetch application dependencies. GoogleTest fetching is enabled by
default. For a completely offline build, install GoogleTest or provide its CMake
package and configure with:

```bash
-DCPGDB_FETCH_TEST_DEPENDENCIES=OFF
```

## GPU configuration

The GPU preset expects package configuration files for RAFT, RMM, and cuGraph.
A typical RAPIDS Conda environment can be supplied through `CMAKE_PREFIX_PATH`:

```bash
cmake --preset gpu-bfs \
  -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" \
  -DCMAKE_CUDA_ARCHITECTURES=86
```

The `cpgdb_gpu_dependencies` interface target ensures that any target receiving
`CPGDB_ENABLE_GPU` also inherits RAFT, RMM, cuGraph, and CUDA include/link data.

## Documentation

```bash
cmake --preset docs
cmake --build --preset docs
```

Open `build/docs/docs/generated/html/index.html` after generation.
