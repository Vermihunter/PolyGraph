# PostgreSQL relational runtime and BFS (#21)

`PostgresRelationalGraphStore` implements the existing `IGraphStore` rich,
minimal and streamed neighbor APIs. It combines
[dedicated-table](postgresql-relational-relationships.md) and
[foreign-key](postgresql-relational-foreign-keys.md) readers, resolving native
keys through an immutable [identity catalog](postgresql-relational-identities.md).
No application tables or generic compatibility views are created or modified.

## Runtime configuration

```json
{
  "idRouting": ["PostgreSQL"],
  "backends": [{
    "id": "PostgreSQL",
    "kind": "PostgreSQL",
    "relationalMapping": "mapping.json",
    "relationalIdentity": "catalog/manifest.json",
    "connection": {
      "endpoint": "127.0.0.1:5432",
      "database": "application_graph"
    },
    "capabilities": {"batch_neighbors": true},
    "coverage": {"mode": "id-routing", "fresh": true},
    "server_side_candidate_deduplication": false
  }]
}
```

Both file references belong on the backend object and resolve against the backend
configuration file, independently of the process working directory. Programmatic
connection configuration supplies absolute paths. `relationalIdentity` without
`relationalMapping`, either field on another backend kind, or placement inside
`connection` is rejected. A missing/mismatched/corrupt catalog fails explicitly;
the factory never substitutes generic SQL for relational mode. Without the
mapping, existing generic PostgreSQL traversal remains selected.

The identity file is the manifest from read-only attachment or successful
relational import/recovery. Its mapping fingerprint, active IDs and authoritative
owners must match the graph being traversed. Files remain immutable while used.
For multiple owners, include all enabled owner backends in `idRouting`, with the
same layout as the source graph metadata, and map all PostgreSQL-owned logical
types needed by that graph.

The endpoint accepts `host:port` or a libpq PostgreSQL URI. An explicit database
and credentials override URI/environment defaults; omitted credentials use libpq
environment/defaults. The adapter connects lazily. Read access needs schema USAGE
and table-level SELECT on mapped tables, including mapped columns. It needs no
application write permissions or PostgreSQL TEMP privilege. Existing RLS and
native-key/type limitations of the readers still apply.

## Traversal semantics

`Directed` follows stored logical from-to orientation; `Both` and `Undirected`
include outgoing and incoming relationships, matching generic PostgreSQL behavior.
Empty filters are wildcards. Known logical types without PostgreSQL mappings
produce no matching candidates; invalid logical filters fail. Table and FK
branches share one read-only repeatable-read snapshot per frontier chunk. NULL
FKs have no adjacency, self-loop incoming duplicates are suppressed, and parallel
physical table rows remain separate when candidate deduplication is disabled.

Frontiers are bounded to at most 4096 unique canonical IDs per reader operation,
respecting smaller `neighbor_batch_size` values. Larger requests split into
chunks. ID-only source refs resolve their logical type from the catalog; duplicate
frontier entries retain separate result rows without multiplying their adjacency.
Cache-residency bits do not change canonical identity. Stale/missing isolated
frontier rows fail even when filters match no relationships.

Rich/minimal APIs materialize their result, as the interface requires. Streaming
emits transient batches no larger than the requested candidate size or 1024,
without retaining complete adjacency. Native relationship attributes and external
edge keys are omitted from BFS projections. Rich `via_edge` remains absent, as in
the generic PostgreSQL store: native edge PKs are not canonical edge IDs. The
dedicated reader APIs remain available when native edge attributes/keys are needed.

Optional candidate deduplication preserves one source/target pair in minimal/rich
results and one target across the streamed request, matching generic DISTINCT
semantics. This relational implementation uses private disk-backed SQLite state
with a bounded cache, not a graph-sized in-memory set or PostgreSQL application
metadata. The existing option name is retained for configuration compatibility.

Each active operation exclusively leases its libpq connection. One idle connection
is cached for reuse; concurrent/reentrant calls use independent connections.
Callbacks run without the idle-slot mutex held. Callback exceptions roll back the
reader snapshot and leave the store reusable. As with other streamed backends,
callback prefixes must be discarded if the request later fails. Concurrent calls
can open additional connections; there is no new PostgreSQL admission scheduler.

## Placement and BFS

Neighbors carry canonical catalog IDs and concrete logical types. Metadata proxy
targets keep their authoritative Neo4j/ArangoDB ownership; they are never returned
as invented application Proxy vertices. The existing placement catalog and BFS
executor route the next frontier to that owner, including a later return to
PostgreSQL. Compatible local reference rows are still required by the native
reader contract; relational import does not fabricate them.

Both predecessor and streamed no-predecessor BFS paths use the same adjacency.
Depth/type filters and visited-set semantics remain in the existing executor.
The adapter does not claim native whole-BFS pushdown or complete pull traversal.
It does not provide one database snapshot spanning a complete BFS or all chunks;
external data changes still require explicit generation refresh under the catalog
contract.

Use an attached catalog with the existing CLI source-root options:

```sh
build/cpu-tests/bin/polygraph_bfs \
  --logical-graph-schema schema.json --data graph.jsonl --backend-config backends.json \
  --start-source '{"namespace":"bfs","type":"Person","idType":"int64","id":7}' \
  --source-index catalog/manifest.json
```

The graph file supplies normal runtime ID-routing metadata. It need not contain a
duplicate copy of application rows for this PostgreSQL adapter.

## Verification

`integration.postgres-relational-bfs` uses an owned PostgreSQL fixture and creates
uniquely named databases. Set `POLYGRAPH_RELATIONAL_TEST_DSN` with CREATEDB and
CREATEROLE permissions; unset DSN returns skip code 77. Coverage includes full
runtime construction, attached source roots, native table/FK adjacency, generic
PostgreSQL BFS parity, directions/filters/depths/predecessors, metadata proxy-owner
handoff, bounded chunks/callbacks, read-only roles, concurrent requests, callback
failure and reentrancy. Cross-owner tests use real PostgreSQL plus deterministic
in-process Neo4j/ArangoDB stores; full live three-service acceptance remains #22.
