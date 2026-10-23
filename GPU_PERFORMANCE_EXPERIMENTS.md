# PolyGraph GPU performance experiments

This source drop keeps the existing single-GPU semantics as the baseline and adds runtime-selectable transfer/result paths so the same benchmark can compare them without maintaining separate branches.


## Default production GPU profile

Normal GPU execution now defaults to the best-performing benchmark profile, equivalent to `v3-compact-1m`:

```json
"gpu_execution": {
  "transfer_mode": "pipelined",
  "d2h_chunk_elements": 1048576,
  "pin_projection_build_inputs": true,
  "candidate_deduplication": true,
  "device_candidate_compaction": true,
  "device_candidate_bitmap_compaction": false,
  "dense_candidate_deduplication": true
}
```

The JSON block is optional because these are now the C++ defaults. Backend JSON and `POLYGRAPH_GPU_*` environment variables still override them, so the benchmark matrix continues to work unchanged.

## 1. Runtime variants

Add `gpu_execution` to a cuGraph backend entry.

### V0 — baseline

```json
"gpu_execution": {
  "transfer_mode": "baseline",
  "pin_projection_build_inputs": false,
  "device_candidate_compaction": false,
  "dense_candidate_deduplication": false
}
```

Preserves the original pageable host buffers, stream synchronization, and host sort/unique path.

### V1 — pinned

```json
"gpu_execution": {
  "transfer_mode": "pinned",
  "pin_projection_build_inputs": true,
  "device_candidate_compaction": false,
  "dense_candidate_deduplication": true
}
```

Uses page-locked host memory for hot H2D/D2H paths. Candidate-only one-hop expansion uses a dense-ID bitmap instead of `std::sort/std::unique`.

### V2 — pipelined

```json
"gpu_execution": {
  "transfer_mode": "pipelined",
  "d2h_chunk_elements": 1048576,
  "pin_projection_build_inputs": true,
  "device_candidate_compaction": false,
  "dense_candidate_deduplication": true
}
```

Double-buffers D2H chunks on two non-blocking CUDA streams. While chunk N+1 is copied, the CPU converts/emits chunk N.

Try `d2h_chunk_elements` = 65536, 262144, 1048576, and 4194304. The optimum depends on PCIe generation, CPU speed, result density, and GPU.

### V3 — device compact + pipelined D2H

```json
"gpu_execution": {
  "transfer_mode": "pipelined",
  "d2h_chunk_elements": 1048576,
  "pin_projection_build_inputs": true,
  "device_candidate_compaction": true,
  "dense_candidate_deduplication": true
}
```

For candidate-only one-hop expansion, sampled destinations are sorted and uniqued on the GPU. Only unique dense IDs cross PCIe. The remaining D2H transfer is pipelined and converted to PolyGraph IDs in bounded batches.

This variant is expected to help most when the sampled edge count is much larger than the unique destination count. If nearly every destination is unique, the device sort can cost more than it saves, which is why it is a separate benchmark mode.

## 2. What changed in the hot path

- `CudaDeviceGuard` selects the correct CUDA device in every worker thread. CUDA's current device is thread-local, which matters because PolyGraph dispatches backend work from asynchronous worker threads.
- Reusable graph projections remain resident on the target GPU.
- Projection-build COO arrays can be temporarily host-registered to improve the one-time H2D upload.
- Pinned host result buffers enable genuinely asynchronous CUDA copies.
- Pipelined result transfer uses two copy streams and CUDA events.
- Candidate-only expansion can avoid CPU `O(E log E)` sort/unique with either an `O(E)` dense-ID bitmap or GPU-side sort/unique.

## 3. Multi-GPU modes

There are two different things worth benchmarking.

### A. PolyGraph-sharded multi-GPU (fits the current architecture)

Create one cuGraph backend per GPU (`GPU:0`, `GPU:1`, ...), give each backend its own placement/projection, and let the existing planner split the frontier. `NeighborExpander` can execute backend expansions concurrently. The new `CudaDeviceGuard` is important here because each asynchronous worker must select the projection's CUDA device before RMM/cuGraph allocations and kernels.

This mode is simple, integrates with heterogeneous placement, and lets different GPUs own different graph regions. Cross-GPU frontier merging still flows through PolyGraph/CPU-side orchestration.

### B. Native cuGraph distributed multi-GPU

Native `cugraph::graph_t<..., multi_gpu=true>` is a different execution model: one cooperative MPI rank per GPU, a RAFT communicator, cuGraph partition subcommunicators, edge shuffling to owning ranks, and graph renumbering. The test harness under `experiments/distributed_gpu/mg_cugraph_bfs.cpp` demonstrates that path independently of the server architecture.

For production integration, the clean boundary is a distributed GPU service/executor that owns the MPI world and exposes graph operations back to PolyGraph. Trying to hide a collective multi-rank cuGraph graph inside a normal single-process `IGraphStore` would make lifecycle, failure, and collective-call ordering much harder to reason about.

## 4. RDMA / GPUDirect RDMA

RDMA does **not** speed ordinary CPU↔GPU H2D/D2H inside one machine. It matters when a NIC transfers data directly to/from GPU memory, primarily for multi-node execution.

The native multi-GPU cuGraph experiment uses RAFT/MPI communication; on a cluster with CUDA-aware MPI/UCX and GPUDirect RDMA configured, the communication layer can use the GPU↔NIC direct path instead of bouncing through pageable CPU memory.

`experiments/distributed_gpu/cuda_aware_mpi_pingpong.cu` is a minimal infrastructure test: MPI receives and sends CUDA device pointers directly. Compare it with a host-buffer MPI ping-pong to verify whether your cluster's CUDA-aware/RDMA path is actually helping.

## 5. Benchmark matrix

Keep graph, source set, placement, warm-up policy, and result semantics fixed.

| Variant | Projection H2D | Frontier H2D | Candidate/result D2H | Dedup |
|---|---|---|---|---|
| V0 baseline | pageable | pageable | pageable, blocking | CPU sort/unique |
| V1 pinned | host-registered | pinned | pinned, blocking | CPU bitmap |
| V2 pipelined | host-registered | pinned | pinned + double-buffered | CPU bitmap while copying |
| V3 compact | host-registered | pinned | only unique IDs + pipeline | GPU sort/unique |
| V4 bitmap | host-registered | pinned | only unique IDs + pipeline | GPU atomic bitmap + compact |
| V5 no dedup | host-registered | pinned | all candidates + pipeline | global BFS visited map only |
| SG x N GPUs | same as selected V1-V5 | per GPU | per GPU | per GPU |
| native MG | distributed/shuffled | distributed | local partitions | cuGraph distributed |

Record at least:

- end-to-end request latency;
- cuGraph kernel/API time;
- H2D bytes and time;
- D2H bytes and time;
- sampled edges vs unique destinations;
- GPU utilization, SM occupancy, DRAM throughput, PCIe TX/RX;
- CPU time spent converting `dense_id -> GlobalId`;
- peak device and pinned-host memory;
- for MG: per-rank time plus bytes over NVLink/PCIe/network.

Nsight Systems is the first profiler to use for overlap: the V2/V3 timeline should visibly show D2H copies overlapping CPU conversion. Nsight Compute is useful only after the timeline shows the GPU kernel itself is the bottleneck.

## 6. Next high-value optimizations after these variants

1. Keep frontier IDs in dense GPU representation for several consecutive GPU-owned BFS levels instead of round-tripping through `GlobalId` every level.
2. Add reusable per-projection scratch buffers for sources/distances/predecessors to remove repeated allocator traffic. Do this only with explicit concurrency ownership (one scratch arena per in-flight query/stream).
3. Configure an RMM async/pool memory resource at process startup and benchmark it separately from algorithmic changes.
4. Add GPU-resident visited/frontier state for multi-level delegated BFS, returning only boundary vertices to PolyGraph.
5. On multi-GPU hosts, benchmark NVLink/P2P-aware placement and peer transfers before involving host memory.
6. On multi-node systems, verify GPUDirect RDMA with the ping-pong test before attributing any graph scaling result to RDMA.

## 7. Reproducible benchmark commands

The patch includes runtime environment overrides so transport variants can be
benchmarked against the same generated cache plan without editing backend JSON:

```text
POLYGRAPH_GPU_TRANSFER_MODE
POLYGRAPH_GPU_D2H_CHUNK_ELEMENTS
POLYGRAPH_GPU_PIN_PROJECTION_BUILD_INPUTS
POLYGRAPH_GPU_CANDIDATE_DEDUPLICATION
POLYGRAPH_GPU_DEVICE_CANDIDATE_COMPACTION
POLYGRAPH_GPU_DEVICE_CANDIDATE_BITMAP_COMPACTION
POLYGRAPH_GPU_DENSE_CANDIDATE_DEDUPLICATION
```

A device-specific value such as `POLYGRAPH_GPU_1_TRANSFER_MODE` overrides the
global value for GPU 1.

Run all single-GPU transport variants on the GPU-heavy SF1 cache configuration:

```bash
RELOAD_STORAGE=1 tools/measurement/run_gpu_transport_matrix.sh 10
```

Run one representative chunk size for the main strategies:

```bash
GPU_VARIANTS=v0-baseline,v1-pinned,v2-pipeline-1m,v3-compact-1m,v4-bitmap-1m,v5-no-dedup-1m \
  tools/measurement/run_gpu_transport_matrix.sh 10
```

`v3-compact-*` is GPU sort/unique, `v4-bitmap-*` is GPU atomic bitmap
compaction, and `v5-no-dedup-*` intentionally forwards duplicates to the
existing global BFS visited map.

Run PolyGraph-level 1/2/4-GPU scaling while preserving the aggregate GPU memory
budget (recommended strong-scaling experiment):

```bash
GPU_SCALE_COUNTS=1,2,4 \
GPU_SCALE_MEMORY_MODE=fixed-total \
GPU_SCALE_VARIANT=v3-compact-1m \
  tools/measurement/run_gpu_scale_matrix.sh 10
```

For a separate capacity-scaling experiment, give every GPU the original
per-device memory budget:

```bash
GPU_SCALE_COUNTS=1,2,4 \
GPU_SCALE_MEMORY_MODE=per-gpu \
GPU_SCALE_VARIANT=v3-compact-1m \
  tools/measurement/run_gpu_scale_matrix.sh 10
```

Native cuGraph MG and RDMA/CUDA-aware MPI matrices are under
`experiments/distributed_gpu/` and are intentionally kept separate from the
PolyGraph HTTP benchmark because they measure a different execution model.

## Next-stage BFS execution experiments

The transport sweep established `v3-compact-1m` (device `sort + unique`, then 1M-element pipelined D2H) as the default one-hop GPU path.  The next benchmark isolates two larger architectural changes rather than continuing to tune the same copy size.

### V6 — persistent per-query GPU visited filter

The normal heterogeneous BFS still returns to the PolyGraph coordinator after each level.  V6 keeps V3's winning `sort + unique`, then applies one CUDA kernel that atomically **tests visited + marks visited + compacts newly seen IDs** against a persistent per-query device bitmap.  The CPU/global visited state remains authoritative, so this is an optimization only; losing/evicting a GPU visited context cannot change correctness.

```
cuGraph one-hop expansion
        -> device sort + unique
        -> persistent GPU visited test/mark/compact
        -> 1M-element pipelined D2H
        -> PolyGraph coordinator
```

Enable with `POLYGRAPH_GPU_DEVICE_QUERY_VISITED_FILTER=true`.

### V7/V8/V9 — native full-projection GPU BFS

`01-gpu-100` contains the whole graph, so it can execute the complete depth-limited traversal with one `cugraph::bfs(...)` call.  cuGraph then owns frontier and visited state on the device for the whole traversal; PolyGraph does not perform a D2H/H2D round trip after every level.

- **V7 native** — native cuGraph BFS, regular result buffers.
- **V8 native reuse** — reuses projection-local distance/predecessor/source device buffers across serial queries.
- **V9 native reuse + compact result** — V8 plus a CUDA reached-vertex compaction kernel before D2H.  This is expected to help when a depth-limited traversal reaches substantially less than half of the projection; it may lose when almost every vertex is reached because each compacted result carries both dense vertex ID and distance.

Existing cache plans do not need regeneration. Runtime loading proves native-BFS eligibility from `artifacts.gpu[device].residentVertices == plan.nodes`. Partial GPU caches remain ineligible.

### Focused benchmark

```bash
GPU_NEXT_OUT=measurements/sf1-gpu-next_n30 \
  tools/measurement/run_gpu_next_optimization_benchmarks.sh 30
```

This compares V3, V6, V7, V8 and V9 on `01-gpu-100`, runs the cached raw-SNB Python reference correctness checks, and writes the usual statistics/plots.

### Optional hybrid-cache benchmark

Only three of the eight SF1 cache configurations contain a GPU: `01`, `04` and `05`.  To measure V6 on the heterogeneous layouts as well:

```bash
GPU_NEXT_RUN_MIXED=1 \
GPU_NEXT_OUT=measurements/sf1-gpu-next_n30 \
GPU_NEXT_MIXED_OUT=measurements/sf1-gpu-next-mixed_n30 \
  tools/measurement/run_gpu_next_optimization_benchmarks.sh 30
```

The mixed run compares V3 vs V6 separately for each GPU-containing cache plan; the analyzer matches speedups against the same cache plan rather than mixing plans.
