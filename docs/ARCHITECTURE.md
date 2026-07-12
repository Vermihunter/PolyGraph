# Architecture {#architecture}

## Goals

CPGDB separates a logical graph from its physical storage. Algorithms operate on
stable graph-domain types while a placement catalog and backend registry decide
which stores can answer each operation. This keeps traversal planning independent
from Neo4j, ArangoDB, Bolt, HTTP, and CUDA implementation details.

## Library graph

```text
cpgdb_project_options
        |
        +--> cpgdb_model
                 |
                 +--> cpgdb_core
                         |
                         +--> cpgdb_backends
                         |       |
                         |       +--> cpgdb_runtime --> cpgdb_cli --> bfs
                         |
                         +--> cpgdb_services
                                 +--> cpgdb_http
                                 +--> cpgdb_grpc
```

When GPU support is enabled, `cpgdb_gpu_dependencies` carries the GPU compile
definition and CUDA/RAFT/RMM/cuGraph usage requirements as one inseparable unit.
No CPU-only target receives `CPGDB_ENABLE_GPU`.

## Source layout

Public declarations are under `include/`. Non-template implementations mirror
the same relative path under `src/`. Templates whose definitions are required at
instantiation time remain in headers.

| Area | Responsibility |
|---|---|
| `data_model` | Nodes, edges, paths, records, variants, and lightweight references |
| `domain/graph` | Logical type schema and property metadata |
| `query` | Backend-neutral query objects and visitor dispatch |
| `data_access` | Store interface and concrete backend adapters |
| `infrastructure` | Registry, capabilities, coverage, placements, and runtime context |
| `algorithms` | Expansion specification and distributed BFS |
| `insertion` | Placement policies, ownership selection, batching, and load tracking |
| `gpu` | Dense ID mapping, CSR projection, GPU expansion, and cuGraph BFS |
| `server` | Transport-neutral service plus HTTP/gRPC adapters |
| `cli` | Configuration parsing, runtime construction, and BFS command line |
| `logging` / `utilities` | Structured events, profiling, curl, UUID, and helpers |

## Query dispatch

`Query` is the backend-neutral command abstraction. Each concrete query accepts a
`QueryVisitor`. A graph store owns or creates the visitor appropriate for its
backend. Read and write operations therefore share one dispatch mechanism.

```text
caller -> IGraphStore::execute(Query)
       -> Query::accept(QueryVisitor)
       -> Neo4JQueryVisitor / ArangoQueryVisitor / other visitor
       -> QueryResult
```

The insertion module adds `InsertQuery` and `InsertBatchQuery` to this same path,
so adding another backend requires implementing visitor methods rather than
changing insertion orchestration.

## Distributed BFS

`BfsExecutor` first asks `PlacementCatalog` whether one backend can answer the
entire traversal natively. Otherwise it performs layered expansion:

1. partition the current frontier by eligible backend;
2. execute independent batch-neighbor queries;
3. merge adjacency rows;
4. deduplicate globally discovered vertices;
5. update distances and optional predecessors;
6. emit an optional streaming layer callback;
7. stop at an empty frontier or the configured depth limit.

The placement catalog considers backend capabilities, placement freshness,
coverage, priority, and whether partial acceleration is allowed.

## Runtime ownership

`DatabaseContext` owns shared runtime services:

- backend registry;
- placement catalog;
- logical graph schema;
- configured backend connections;
- optional GPU runtime state.

Long-lived services receive the context by reference or shared pointer. Query
visitors keep a context reference so backend behavior can consult the same schema
and placement metadata.

## Error model

Core code uses exceptions for invalid configuration, unsupported operations,
backend failures, malformed responses, and violated invariants. The HTTP layer
maps known failures to structured responses. The gRPC layer converts service
failures to gRPC status codes. CLI entry points report errors and return a
non-zero process status.

## Threading

Distributed frontier expansion may execute backend requests concurrently. Shared
registries are read-mostly after construction. `InMemoryBackendLoadTracker` uses a
mutex because insertion accounting can be updated from multiple request threads.
Backend clients must obey their own thread-safety guarantees.
