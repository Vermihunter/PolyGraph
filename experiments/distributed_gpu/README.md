# Distributed GPU / RDMA experiments

These files are deliberately isolated from the main server build. Native libcugraph multi-GPU execution is collective: every rank must enter graph construction and BFS in compatible order.

## Native multi-GPU cuGraph BFS

`mg_cugraph_bfs.cpp` follows the current libcugraph MG execution model:

1. one MPI rank per GPU;
2. rank-local `cudaSetDevice`;
3. RAFT MPI communicator initialization;
4. cuGraph partition subcommunicator initialization;
5. rank-local edge generation/upload;
6. `shuffle_ext_edges` to the owning GPU ranks;
7. `create_graph_from_edgelist<..., multi_gpu=true>` with `renumber=true`;
8. BFS output sized to `local_vertex_partition_range_size()`.

Example launch after adding the file to a RAPIDS/cuGraph-enabled CMake target:

```bash
mpirun -np 4 ./polygraph-mg-bfs 100000000 8 20
```

The program reports the maximum rank time for each collective BFS iteration, which is the meaningful distributed latency.

## GPUDirect RDMA infrastructure test

Compile `cuda_aware_mpi_pingpong.cu` with your cluster's CUDA-aware MPI wrapper/toolchain and run exactly two ranks, preferably on different hosts:

```bash
mpirun -np 2 -H gpu-node-a:1,gpu-node-b:1 ./cuda-aware-mpi-pingpong $((64*1024*1024)) 100
```

The code passes CUDA device pointers directly to MPI. That proves CUDA-aware MPI usage, but not by itself that the chosen network path was GPUDirect RDMA. Check UCX/MPI transport diagnostics and compare against a host-buffer baseline.

Run `check_gpu_fabric.sh` on each node before benchmarking.

## PolyGraph integration recommendation

Keep two implementations:

- **sharded SG mode:** the normal PolyGraph process owns several independent `GPU:n` stores. This is best for heterogeneous placement and is already compatible with the request planner;
- **native MG mode:** a dedicated distributed executor/service owns the MPI communicator and a `multi_gpu=true` graph. PolyGraph sends it a high-level BFS request, not per-edge RPCs.

The second design also gives RDMA a useful place to matter: cuGraph/RAFT communication can remain in device memory across nodes instead of forcing PolyGraph to serialize every frontier through host memory.

## Benchmark build + matrix commands

The experiment binaries can be built from the normal PolyGraph build when GPU
support is enabled:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DPOLYGRAPH_ENABLE_GPU=ON \
  -DPOLYGRAPH_BUILD_DISTRIBUTED_GPU_EXPERIMENTS=ON
cmake --build build -j --target \
  polygraph-host-mpi-pingpong \
  polygraph-cuda-aware-mpi-pingpong \
  polygraph-mg-cugraph-bfs
```

Native cuGraph strong scaling, fixed synthetic graph:

```bash
MG_COUNTS=1,2,4 \
MG_VERTICES=100000000 \
MG_DEGREE=8 \
MG_ITERATIONS=20 \
experiments/distributed_gpu/run_native_mg_matrix.sh
```

For GPUDirect/RDMA, compare host-buffer MPI with CUDA-device-buffer MPI. On one
machine this primarily validates CUDA-aware MPI; use two RDMA-connected nodes to
measure the network path:

```bash
MPI_LAUNCH='mpirun -np 2 -H gpu-node-a:1,gpu-node-b:1' \
RDMA_ITERATIONS=100 \
experiments/distributed_gpu/run_rdma_matrix.sh
```

The CUDA-aware result proves that MPI accepted device pointers. Confirming that
the transfer actually used GPUDirect RDMA still requires checking the selected
UCX/Open MPI transport and the machine's GPU/NIC topology.
