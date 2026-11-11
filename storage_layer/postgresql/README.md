# PostgreSQL storage service

The service is defined in `storage_layer/docker-compose.yml` and initialized by
`init/001_polygraph.sql`.

Default connection:

```text
host:     127.0.0.1
port:     5432
database: polygraph
user:     polygraph
password: polygraph
```

## Benchmark index policy

The default PolyGraph PostgreSQL benchmark is intentionally **without secondary
traversal indexes**. The node/edge primary keys remain because stable IDs are the
loader's identity/upsert key.

During a reset load the Python loader drops any old traversal indexes before
COPY, bulk-loads the graph, and runs `ANALYZE`. To opt into the traversal indexes
for an indexed comparison, add `--postgres-indexes` to `polygraph.py load` or
`polygraph.py instance ... load`.

The C++ BFS adapter bulk-COPYs each frontier into a temporary table and lets
PostgreSQL use a set-oriented join. It does not build a temporary B-tree per BFS
level; this avoids repeated index-build overhead in the default index-free mode.

For manual experiments the equivalent SQL is in
`optional/traversal_indexes.sql`.

## Existing relational schemas

Use explicit mappings and an immutable identity catalog to attach populated
application tables or import into pre-created typed tables. Both junction and
direct-FK adjacency work with distributed BFS. See the
[relational example and live acceptance](../../docs/examples/relational-postgresql/README.md)
and [mapping contract](../../docs/architecture/postgresql-relational-mapping.md).
This directory's default initialization still selects generic storage; relational
mode never migrates or alters application tables.
