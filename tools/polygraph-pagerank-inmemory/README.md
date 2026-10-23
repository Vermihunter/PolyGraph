# PolyGraph PageRank — Neo4j GDS vs Arango Graph Analytics

This bundle compares **the same whole-graph algorithm, PageRank**, on the
in-memory analytics representations of Neo4j and Arango.

## What is compared

### Neo4j

```text
Neo4j persistent graph
        |
        | gds.graph.project()
        v
Neo4j GDS in-memory graph
        |
        | gds.pageRank.stats()
        v
PageRank result/timing
```

### Arango

```text
ArangoDB graph
        |
        | Graph Analytics /v1/loaddata
        v
Arango Graph Analytics Engine in-memory graph
        |
        | /v1/pagerank
        v
PageRank job/timing
```

The Arango Graph Analytics Engine holds imported graph data in main memory.
It is a separate analytics compute engine, not ordinary AQL running directly
inside the standalone ArangoDB server.

## IMPORTANT Arango requirement

The Arango scripts **do not work against a plain local `arangodb:3.12`
container by itself**.

You need access to an Arango Graph Analytics Engine from either:

- Arango Contextual Data Platform, or
- Arango Managed Platform (AMP).

Set:

```bash
export ARANGO_ENGINE_URL='https://.../v1-parent-without-v1'
export ARANGO_TOKEN='...'
```

Examples of `ARANGO_ENGINE_URL`:

Contextual Data Platform:

```text
https://<EXTERNAL_ENDPOINT>:8529/gral/<SERVICE_ID_POSTFIX>
```

AMP:

```text
https://<APPLICATION_ENDPOINT>:8829/graph-analytics/engines/<ENGINE_ID>
```

The engine must belong to / have access to the ArangoDB deployment that
contains the full LDBC graph.

## Fair PageRank parameters

Both sides use:

```text
directed graph
unweighted PageRank
damping factor = 0.85
maximum iterations = 20
```

Neo4j additionally uses:

```text
tolerance = 1e-7
```

which matches the convergence threshold documented by Arango Graph Analytics.

The Neo4j projection uses `NATURAL` relationship orientation. This is
deliberately different from the earlier BFS projection, which was UNDIRECTED.

## Expected PolyGraph data

The scripts expect your existing prepared domains:

```text
data/ldbc-snb/sf1_0/native-neo4j/
data/ldbc-snb/sf1_0/native-arango/
```

and analogously for SF10, SF30, etc.

---

# Neo4j

## 1. Project the full graph into GDS memory

```bash
SF=1.0 ./prepare_neo4j_pagerank.sh
```

This creates:

```text
polygraph_pagerank_sf1_0
```

It is kept in GDS memory until dropped or Neo4j restarts.

## 2. Benchmark PageRank

```bash
SF=1.0 REPS=10 ./run_neo4j_pagerank.sh
```

Example CSV:

```text
backend,rep,damping,max_iterations,ran_iterations,converged,compute_ms,http_seconds
neo4j-gds,1,0.85,20,20,true,431,0.486321
...
```

`compute_ms` is GDS's own algorithm compute time.

---

# Arango Graph Analytics

## 1. Configure engine

```bash
export ARANGO_ENGINE_URL='https://host:8829/graph-analytics/engines/ENGINE_ID'
export ARANGO_TOKEN='TOKEN'
export ARANGO_DATABASE='university'
```

If the endpoint uses a self-signed certificate:

```bash
export CURL_INSECURE=1
```

## 2. Load graph topology into Graph Analytics memory

```bash
SF=1.0 ./prepare_arango_pagerank.sh
```

The script reads all node and relationship collection names from the
PolyGraph LDBC schema and sends:

```json
{
  "database": "university",
  "vertex_collections": ["..."],
  "edge_collections": ["..."],
  "vertex_attributes": []
}
```

No properties are loaded because PageRank only needs topology.

After load completion, it stores the `graph_id` in:

```text
.pagerank-state/arango-sf1_0.env
```

## 3. Benchmark PageRank

```bash
SF=1.0 REPS=10 ./run_arango_pagerank.sh
```

Example CSV:

```text
backend,rep,damping,max_iterations,job_id,job_seconds
arango-gae,1,0.85,20,123,0.927135
...
```

Each final job JSON response is stored under:

```text
results/arango/
```

This lets you inspect engine-specific timing/status fields later.

---

# Run both

Once the two in-memory graphs have been prepared:

```bash
SF=1.0 REPS=10 ./run_both_pagerank.sh
```

Results go to:

```text
results/neo4j.csv
results/arango.csv
```

## Variables

Common:

```text
SF=1.0
REPS=10
DAMPING=0.85
MAX_ITERATIONS=20
```

Neo4j:

```text
GDS_CONCURRENCY=4
GDS_GRAPH_NAME=polygraph_pagerank_sf1_0
GDS_REPROJECT=0
```

Arango:

```text
ARANGO_ENGINE_URL=...
ARANGO_TOKEN=...
ARANGO_DATABASE=university
ARANGO_GRAPH_ID=...          # optional override
ARANGO_LOAD_PARALLELISM=4
ARANGO_LOAD_BATCH_SIZE=400000
JOB_POLL_INTERVAL=1
JOB_TIMEOUT=3600
CURL_INSECURE=0
```

## Why PageRank?

It is a useful PolyGraph benchmark because it:

- touches the whole graph repeatedly,
- has multiple iterations,
- exercises memory bandwidth and graph topology access,
- parallelizes well,
- exists natively in both Neo4j GDS and Arango Graph Analytics,
- avoids the unfair comparison of Neo4j GDS BFS against Arango storage-level AQL traversal.

Other useful common algorithms after PageRank are WCC and SCC.
