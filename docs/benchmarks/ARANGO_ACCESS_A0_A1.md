# ArangoDB A0/A1 BFS access experiment

This experiment keeps the native ArangoDB graph representation unchanged and
changes only the one-hop access primitive PolyGraph uses during distributed BFS.

## Modes

- `traversal` (A0): one-hop AQL graph traversal. Arango resolves the start
  vertex and materializes traversal neighbor vertices.
- `edge-index` (A1): direct queries against native edge collections using the
  built-in `_from` / `_to` edge index. The no-predecessor hot path returns only
  the destination document handle and logical target type; it does not request
  neighbor vertex documents.
- A1 can additionally set `arangoUsePlanCache=true`. This enables ArangoDB's AQL
  execution-plan cache for the repeated direct edge-index query shapes. The
  query result cache remains disabled because BFS frontier values change.

The stored graph, edge collections, vertex collections, and placement metadata
are identical in all modes.

## HTTP request knobs

```json
{
  "arangoExpandMode": "edge-index",
  "arangoUsePlanCache": true
}
```

`arangoExpandMode` accepts `traversal` or `edge-index`.

## CLI knobs

```bash
--arango-expand-mode traversal
--arango-expand-mode edge-index
--no-arango-plan-cache
```


## Bounded A0 traversal batches

A0 graph traversal must not reuse the very large frontier batching that is useful for A1.
The HTTP/CLI option `arangoTraversalBatchSize` / `--arango-traversal-batch-size` is therefore
separate from `neighborBatchSize`. In traversal mode PolyGraph always uses bounded chunked
requests, so changing A0 batch size does not perturb Neo4j or the A1 edge-index path.

The benchmark runner sweeps 256, 1024, and 4096 source vertices per A0 traversal query by
default, then runs A1 with and without the plan cache. This avoids the pathological case where
`neighborBatchSize=250000` plus `arangoRequestStrategy=auto` can turn a large source collection
frontier into one enormous multi-source graph-traversal query.

## Benchmark

Run the three A0 batch sizes plus both A1 variants over the normal SF1 cache matrix:

```bash
ARANGO_ACCESS_OUT=measurements/sf1-arango-access_n30 \
  tools/measurement/run_arango_access_benchmark.sh 30
```


The runner defaults to a 600-second per-BFS HTTP timeout (`ARANGO_ACCESS_REQUEST_TIMEOUT_SECONDS`).
A timed-out variant is recorded as failed, but the runner continues with the remaining variants so an
unattended experiment still produces the A1 results and any successful A0 batches. Set the variable to
`0` to restore the normal unlimited request timeout. At the end, `_analysis/summary.md` and
`_analysis/statistics.csv` are generated automatically.

To run only the two access primitives without plan-cache A/B:

```bash
ARANGO_ACCESS_VARIANTS=a0-traversal-256,a0-traversal-1k,a0-traversal-4k,a1-edge-index \
  tools/measurement/run_arango_access_benchmark.sh 30
```

The runner generates the request/config files it used under
`<output>/_generated/` and stores every variant in a separate output directory.

## Verify the A1 query plan

The existing EXPLAIN helper checks that the hot query contains an `IndexNode`
and no full edge-collection scan:

```bash
python3 tools/measurement/arango_explain.py \
  --database <database> \
  --edge-collection <edge-collection> \
  --vertex-collection <vertex-collection> \
  --vertex-id <global-id> \
  --direction outbound
```
