# Troubleshooting {#troubleshooting}

## `raft/core/handle.hpp` not found

Use a CPU preset unless GPU support is intended:

```bash
cmake --preset cpu-bfs --fresh
```

For GPU mode, ensure RAFT, RMM, and cuGraph are installed and visible:

```bash
cmake --preset gpu-bfs --fresh -DCMAKE_PREFIX_PATH="$CONDA_PREFIX"
```

Do not define `CPGDB_ENABLE_GPU` manually. CMake supplies it only through
`cpgdb_gpu_dependencies`, together with all required include directories.

## clang-format or clang-tidy not found

Install matching LLVM tools or provide explicit paths:

```bash
-DCPGDB_CLANG_FORMAT_EXECUTABLE=/usr/bin/clang-format-21
-DCPGDB_CLANG_TIDY_EXECUTABLE=/usr/bin/clang-tidy-21
```

Disable gates only for diagnostic builds.

## GoogleTest cannot be downloaded

Install GoogleTest and set `GTest_DIR`, or configure tests off:

```bash
-DCPGDB_BUILD_TESTS=OFF
```

For offline tests, also set `CPGDB_FETCH_TEST_DEPENDENCIES=OFF` after installing
GoogleTest locally.

## libcurl conflicts with a Conda library

Prefer one consistent dependency prefix. In a RAPIDS environment, point CMake at
Conda and avoid mixing `/usr/lib` and `$CONDA_PREFIX/lib` versions of libcurl.
Inspect selected libraries with:

```bash
cmake --preset gpu-bfs --debug-find-pkg=CURL
```

## gRPC reflection is unavailable

The project implements the traversal service but does not automatically enable
gRPC server reflection. Clients that query descriptors dynamically need the
reflection plugin registered in the server, or must load `proto/bfs_stream.proto`
locally.

## Doxygen fails on Graphviz

Doxygen can generate HTML without diagrams by setting `HAVE_DOT=NO` in the
configured Doxyfile. For the supplied configuration, install Graphviz so SVG
class and collaboration diagrams can be generated.
