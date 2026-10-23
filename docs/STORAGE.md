# Storage backends and inspection

PolyGraph currently integrates persistent Neo4j, ArangoDB, and PostgreSQL stores plus acceleration stores (GPU/cuGraph, Redis, mmap).

## Neo4j

Real Neo4j entities use native database IDs for local lookup. Proxy/replicated entities retain global-ID metadata where needed by the distributed graph representation. Neighbor expansion is batched and may use native pushdown/GDS when capabilities and coverage allow it.

Useful count query:

```cypher
MATCH (n)
RETURN labels(n) AS labels, count(*) AS nodes
ORDER BY nodes DESC;
```

For native BFS/GDS experiments, create a projection consistent with the benchmark's relationship orientation and compare vertex counts/results to PolyGraph before treating it as a reference.

## ArangoDB

Vertices/edges use global numeric IDs for routing and direct document lookup. Batch expansion uses AQL graph traversal over the configured named graph/collections.

Example shape:

```aql
FOR item IN @frontier
  LET start = DOCUMENT(item.collection, item.key)
  FILTER start != null
  FOR neighbor, edge IN 1..1 ANY start GRAPH @graphName
    RETURN { start: start.uuid, neighbor: neighbor.uuid, edge: edge.uuid }
```

## PostgreSQL

PostgreSQL stores vertices and relationships in relational tables with covering indexes for both edge directions. Large frontier expansion uses binary `COPY` into a temporary frontier relation followed by an indexed join. The no-predecessor BFS path consumes the libpq result in single-row mode and streams candidate batches directly into the global visited/frontier state. See [POSTGRESQL.md](POSTGRESQL.md).

## Redis

Redis is a generated adjacency cache, not authoritative storage. Keys are canonical source IDs under the configured prefix; values are packed uint64 neighbor arrays. A readiness manifest gates runtime use and binds the materialization to the selected cache plan.

## Memory-mapped cache

MMAP artifacts use generated `index.bin`/`neighbors.bin` CSR-like adjacency. Their path is carried by the selected cache plan runtime backend.

## GPU/cuGraph

The GPU store is constructed in-process from the canonical graph and selected external cache residency. It is not a separately loaded persistent database in the recommended cache workflow.
