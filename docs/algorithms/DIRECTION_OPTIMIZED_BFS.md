# Direction-optimized BFS

PolyGraph exposes two distributed BFS execution policies:

- `classic`: the historical level-synchronous top-down/push BFS.
- `direction-optimized`: an adaptive exact BFS that can switch between top-down push and bottom-up pull and between sparse and bitmap frontier membership.

Native whole-BFS pushdown still has priority when `allowNativePushdown=true`. Set it to `false` when comparing the two distributed policies.

## Correctness rule for pull

A bottom-up step scans unvisited vertices and asks whether any incident neighbor is in the current frontier. This is exact only when the scanning backend contains the complete candidate-vertex universe and complete adjacency for the requested expansion. PolyGraph therefore never performs pull against a partial cache.

The first pull-capable implementation is a truly full memory-mapped adjacency cache. `03-mmap-100` now opts into caching GPU-owned vertices as well, and it is recognized as complete only when its artifact vertex count equals the graph node count. Rebuild that cache plan once after applying this patch; older `03-mmap-100` artifacts may intentionally omit GPU-owned vertices and will therefore remain push-only.

Partial MMap/GPU/Redis cache configurations remain legal with `direction-optimized`; they simply stay in push mode unless a complete pull-capable backend is present.

The current MMap pull implementation supports the same cache semantics as normal MMap expansion: unfiltered `Direction::Both` traversal without edge references.

## Direction policy

The current heuristic uses frontier density over the complete pull universe with hysteresis:

- enter pull when `frontier / |V| >= pullEnterFrontierPercent` (default 5%),
- return to push when `frontier / |V| <= pullExitFrontierPercent` (default 1%).

This deliberately keeps the policy simple and benchmarkable. The thresholds are runtime parameters, so a later cost model or Beamer-style edge-work heuristic can replace the decision function without changing backend APIs.

## Frontier representation

For membership tests the adaptive executor chooses:

- sparse: sorted GlobalId vector + binary search,
- bitmap: one bit per dense BFS state entry.

Bitmap membership is selected when both:

- frontier size >= `bitmapFrontierMinSize` (default 4096), and
- frontier / currently-known BFS vertices >= `bitmapFrontierDensityPercent` (default 10%).

The sparse `NodeRef` list is retained for backend push APIs; the adaptive representation is used for the membership-heavy part of pull execution. This keeps existing backend contracts unchanged while making the new policy selectable and incremental.

## HTTP

Classic:

```json
{
  "allowNativePushdown": false,
  "executionPolicy": "classic"
}
```

Direction optimized:

```json
{
  "allowNativePushdown": false,
  "executionPolicy": "direction-optimized",
  "pullEnterFrontierPercent": 5,
  "pullExitFrontierPercent": 1,
  "bitmapFrontierMinSize": 4096,
  "bitmapFrontierDensityPercent": 10
}
```

## CLI

```bash
--bfs-execution-policy classic
```

or:

```bash
--bfs-execution-policy direction-optimized \
--pull-enter-frontier-percent 5 \
--pull-exit-frontier-percent 1 \
--bitmap-frontier-min-size 4096 \
--bitmap-frontier-density-percent 10
```

## Events

The adaptive path emits events that make policy behavior reconstructable from `events.json`:

- `full_bfs.direction_optimized`
- `bfs.direction_decision`
- `bfs.expand_layer.push`
- `bfs.expand_layer.push.candidate_streaming`
- `bfs.expand_layer.pull`
- `mmap.pull_candidates`

`bfs.direction_decision` records the level, frontier size, direction, frontier representation, densities, and selected pull backend.

## First apples-to-apples benchmark

Use full MMap coverage because it can execute both exact push and exact pull. The included benchmark configs disable native BFS pushdown. Rebuild the cache plans once so `03-mmap-100` contains every owner class:

```bash
python3 tools/cache/build_benchmark_plans.py --sf 1 --distribution schema-based
```

Classic:

```bash
CFG=config/benchmarks/sf1-bfs-classic-mmap100.json \
OUT=measurements/sf1-bfs-policy/classic-mmap100 \
tools/measurement/benchmark.sh 10
```

Direction optimized:

```bash
CFG=config/benchmarks/sf1-bfs-direction-optimized-mmap100.json \
OUT=measurements/sf1-bfs-policy/direction-optimized-mmap100 \
tools/measurement/benchmark.sh 10
```
