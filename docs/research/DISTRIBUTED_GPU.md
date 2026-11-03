# Native distributed GPU exploration

**Status: research programs in the source tree; not integrated into the BFS runtime.**

This page prevents an easy architectural mistake: **multiple GPU stores in one PolyGraph process** and **a collective multi-GPU cuGraph graph** are not the same thing. The former is part of the current heterogeneous placement model. The latter requires a coordinated MPI rank group, a partitioned graph, and collective participation in each traversal.

## What is implemented

| Path | Location | Role | Runtime status |
|:--|:--|:--|:--|
| Existing GPU projection / BFS | `include/gpu/`, `src/gpu/`, backend adapters | Local GPU execution and sharded residency | Optional main runtime |
| Native multi-GPU cuGraph BFS | [`mg_cugraph_bfs.cpp`](../../src/gpu/distributed/mg_cugraph_bfs.cpp) | Synthetic fixed-graph strong-scaling probe | Opt-in standalone |
| Host MPI ping-pong | [`host_mpi_pingpong.cpp`](../../src/gpu/distributed/host_mpi_pingpong.cpp) | Network / host-buffer baseline | Opt-in standalone |
| CUDA-aware MPI ping-pong | [`cuda_aware_mpi_pingpong.cu`](../../src/gpu/distributed/cuda_aware_mpi_pingpong.cu) | Device-buffer transport diagnostic | Opt-in standalone |
| Hardware + benchmark harness | [`src/gpu/distributed/README.md`](../../src/gpu/distributed/README.md) | MPI/CUDA topology and matrix runs | Opt-in standalone |

## Why this is not a production backend yet

The native MG prototype works on its own synthetic rank-local graph and does not consume PolyGraph's authoritative dataset or cache-plan schema. It does not implement `IGraphStore`, participate in `PlacementCatalog` routing, or accept a production BFS request from `GraphRuntimeBuilder`. Treating it as a drop-in store would conceal hard correctness and lifecycle problems.

The CUDA-aware ping-pong test establishes whether the MPI library handles CUDA device pointers. **It does not establish that transfers used GPUDirect RDMA.** That requires measuring and inspecting the selected network transport on real multi-node GPU hardware.

## Evidence-driven promotion gates

Before integrating native MG into the runtime:

1. **Correctness** — import canonical IDs and real domain edges; compare output/distance for the same roots against an independent CPU reference, including empty partitions and disconnected graphs.
2. **Lifecycle** — define a collective-safe graph-load/reuse strategy, request rendezvous, timeout/failure behavior, GPU memory bounds, and graceful shutdown across every rank.
3. **Planner contract** — model MG as a coarse-grained executor of an entire compatible traversal, not a per-frontier `IGraphStore` call; preserve filtering, direction, and authoritative ownership semantics.
4. **Transport evidence** — run host MPI vs device MPI on the same graph/hardware and verify UCX/NIC paths before making RDMA claims.
5. **Measured trade-off** — compare with PolyGraph's sharded single-GPU (SG) execution using identical graphs, total GPU memory budgets, root sets, and concurrency.

Until these gates pass, **keep these files compiled only with** `POLYGRAPH_BUILD_DISTRIBUTED_GPU_EXPERIMENTS=ON`. Production presets retain the flag `OFF`.

## How to build and reproduce

See [the source-level README](../../src/gpu/distributed/README.md) for the three CMake targets and matrix commands, and [GPU performance experiments](GPU_PERFORMANCE_EXPERIMENTS.md) for prior single-/multi-GPU optimization context. These runs require CUDA, an MPI-capable environment, and RAPIDS/cuGraph versions compatible with the source.
