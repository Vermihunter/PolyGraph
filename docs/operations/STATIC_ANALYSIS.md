# Static analysis and formatting

CMake presets enable compile command export and support clang-format/clang-tidy through the repository quality configuration.

Typical flow:

```bash
cmake --preset cpu-bfs
cmake --build --preset cpu-bfs
```

The configured tool paths are visible in `CMakePresets.json`. Change them for your distribution if the specified version is unavailable.

Warnings are treated seriously in performance-sensitive code. In hot paths prefer avoiding unnecessary temporary strings/allocations, preserve const-correctness, and keep ownership explicit.

For CUDA code, validate both host compiler diagnostics and NVCC/device build diagnostics; CPU-only syntax success is not a substitute for the GPU preset.
