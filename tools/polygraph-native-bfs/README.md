# PolyGraph native BFS baseline v3

This version uses **`schema-based`** as the PolyGraph instance distribution.

That matches the containers you already have running, for example:

```text
polygraph-sf1-0-schema-based-neo4j-1
polygraph-sf1-0-schema-based-arangodb-1
```

The full-replica artifacts remain separate:

```text
data/ldbc-snb/sf1_0/native-neo4j/
data/ldbc-snb/sf1_0/native-arango/
```

## Important distinction

We do **not** use:

```bash
python3 tools/polygraph.py instance sf1_0 schema-based load
```

for the native full-replica load. That command loads the ordinary
`data/ldbc-snb/sf1_0/schema-based/domain.json`, i.e. your distributed placement.

Instead the script uses the lower-level loader directly:

```bash
python3 tools/load_graph.py ... --only-source Neo4J --reset
```

or:

```bash
python3 tools/load_graph.py ... --only-source ArangoDB --reset
```

Thus the **containers are schema-based**, while each benchmark database gets the
complete graph.

## Your current SF1 state

You already generated the expensive native graph files, so just run:

```bash
START_DB=never SF=1.0 ./prepare_native_storage.sh neo4j
START_DB=never SF=1.0 ./prepare_native_storage.sh arango
```

The conversion step will be skipped.

### Warning

The loader uses `--reset` for the selected backend. Therefore these commands
replace the existing contents of the currently running schema-based Neo4j and
ArangoDB instances with complete graph replicas.

If you want the ordinary distributed `schema-based` PolyGraph placement again
afterwards, reload the ordinary schema-based dataset.

## BFS

Neo4j:

```bash
ROOT_LDBC_ID=<same-person-id> \
SF=1.0 DEPTH=10 REPS=10 \
./benchmark_native_bfs.sh neo4j
```

ArangoDB:

```bash
ROOT_LDBC_ID=<same-person-id> \
SF=1.0 DEPTH=10 REPS=10 \
ARANGO_WARM_MODE=none \
./benchmark_native_bfs.sh arango
```

ArangoDB with full scan + BFS warm-up:

```bash
ROOT_LDBC_ID=<same-person-id> \
SF=1.0 DEPTH=10 REPS=10 \
ARANGO_WARM_MODE=full-scan \
./benchmark_native_bfs.sh arango
```

Change only `SF` for other scale factors.


## v4: ID-routing validation fix

The LDBC graph IDs were generated with this routing layout:

```json
["Neo4J", "ArangoDB", "GPU"]
```

The storage-only `backends.json` may have GPU disabled. The current
`load_graph.py` validates all routing slots before applying `--only-source`,
which previously caused:

```text
ValueError: ID routing backends are not enabled: ['GPU']
```

v4 creates a **temporary loader-only backend config** which:

- preserves the original `idRouting` order exactly;
- marks routing entries enabled so ID decoding remains valid;
- disables any CUDA requirement in that temporary config;
- still calls `load_graph.py --only-source Neo4J` or `ArangoDB`;
- never attempts to load anything into GPU.

Do not solve this by changing Arango's routing layout to just `["ArangoDB"]`.
The existing IDs encode ArangoDB as routing slot 1, so reordering the routing
layout would change their interpretation.


## v5 fixes

### Neo4j: APOC preflight + installer

The benchmark deliberately uses `apoc.path.expandConfig` rather than a generic
variable-length Cypher path enumeration. If APOC Core is not active, run:

```bash
SF=1.0 ./install_neo4j_apoc.sh
```

The helper copies the matching APOC Core JAR already bundled in the Neo4j
Docker image from `/var/lib/neo4j/labs` to `/plugins`, restarts the existing
container, and verifies that `apoc.path.expandConfig` is registered.

Then run:

```bash
SF=1.0 DEPTH=10 REPS=10 ./benchmark_native_bfs.sh neo4j
```

### ArangoDB: fixed bind-variable JSON

The previous helper used:

```bash
vars="${2:-{}}"
```

which is syntactically ambiguous in Bash and can append an extra `}` when a
second argument is supplied. v5 builds the default `{}` separately, so
`jq --argjson` now receives valid bind variables.


## v6: Neo4j uses GDS BFS instead of APOC

The current Neo4j container already loads Graph Data Science successfully.
The native Neo4j benchmark therefore no longer needs APOC.

First project the complete database graph into the GDS in-memory graph catalog:

```bash
SF=1.0 ./prepare_neo4j_gds.sh
```

The projection:

- includes all Neo4j nodes;
- includes every LDBC relationship type;
- projects every relationship as `UNDIRECTED`;
- is stored in memory under `polygraph_native_sf1_0` by default;
- is reused across BFS repetitions;
- disappears when Neo4j is restarted.

Then benchmark:

```bash
SF=1.0 DEPTH=10 REPS=10 ./benchmark_native_bfs.sh neo4j
```

`benchmark_native_bfs.sh` will automatically create the GDS projection if it
does not already exist.

The measured request runs:

```cypher
MATCH (start:Person {ldbc_id:$root})
WITH id(start) AS sourceId
CALL gds.bfs.stream($graphName, {
  sourceNode: sourceId,
  maxDepth: $depth,
  concurrency: $concurrency,
  logProgress: false
})
YIELD nodeIds
RETURN size(nodeIds) AS reached
```

Only the scalar visited-node count is returned over HTTP.

The one-time GDS graph projection is deliberately outside the measured BFS
repetitions.


## v7: GDS `maxDepth` Integer/Long fix

With the Neo4j transactional HTTP endpoint, a small JSON number such as `10`
can reach a GDS configuration map as a Java `Integer`. The installed GDS BFS
implementation expects `maxDepth` as a Java `Long`.

v7 deliberately sends `DEPTH` as a string and converts it at the Cypher
boundary:

```cypher
maxDepth: toInteger($depth)
```

Neo4j's Cypher `Integer` is a signed 64-bit integer, so GDS receives the type it
expects.

No database reload and no GDS re-projection is required for this change.


## v8: ArangoDB whole-graph traversal memory fix

The previous query unnecessarily emitted all three traversal variables:

```aql
FOR v, e, p IN ...
```

even though the benchmark only counts reached vertices. v8 uses the lean
traversal:

```aql
FOR v IN 0..10 ANY @startId ...
  OPTIONS {
    order: "bfs",
    uniqueVertices: "global",
    uniqueEdges: "none",
    parallelism: 1,
    useCache: false
  }
  COLLECT WITH COUNT INTO reached
  RETURN reached
```

The important changes are:

- no edge object
- no path object
- no per-path edge uniqueness state
- one traversal thread for a single BFS source
- Arango edge cache disabled by default for this huge traversal
- explicit 8 GiB AQL per-query memory limit

The memory limit is a safety rail. If the optimized query unexpectedly needs
more than 8 GiB, Arango should return a resource-limit error rather than being
allowed to consume the entire machine.

Override it if needed:

```bash
ARANGO_QUERY_MEMORY_LIMIT=$((12 * 1024 * 1024 * 1024)) ...
```

To deliberately test Arango's edge cache:

```bash
ARANGO_USE_CACHE=true ...
```

For the main large-graph BFS baseline, the recommended default is
`ARANGO_USE_CACHE=false`.

If a request loses the server connection, v8 prints container/OOM diagnostics.
You can also run:

```bash
SF=1.0 ./diagnose_arango_memory.sh
```
