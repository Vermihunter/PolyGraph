# Relational PostgreSQL: attach, import and traverse

PolyGraph can read and import native typed tables alongside its existing generic
PostgreSQL storage. Select the relational adapter explicitly with a versioned
mapping and an immutable identity catalog. Application primary keys stay in their
own columns; PolyGraph IDs, partitions and proxies stay in side metadata.

The small [schema](schema.json), [native DDL](schema.sql) and [mapping](mapping.json)
illustrate `Person` bigint, `Post` UUID and `Comment` text keys, a junction table
with attributes and a direct FK. Create these application tables yourself;
attachment and ingestion never create or alter them.

## Run the complete acceptance fixture

From the repository root, with Docker and the cached repository test images:

```sh
cmake --preset cpu-tests -DPOLYGRAPH_INGESTION_LIVE_TESTS=ON
cmake --build --preset cpu-tests -j 2
ctest --test-dir build/cpu-tests -R '^integration.postgres-relational$' --output-on-failure
```

This opt-in test creates fresh, ownership-labelled PostgreSQL 17, Neo4j 5.26 and
ArangoDB 3.12 containers on random loopback ports, with separate bind mounts and
ephemeral credentials. It never imports into existing services or named volumes.
It removes only its owned containers/network, retains private logs and reports
under `build/cpu-tests/relational-live/`, and checks existing Docker resources are
preserved. Missing prerequisites or an unavailable service fail the requested run;
they never count as skipped acceptance. Neo4j Enterprise requires the operator's
existing license entitlement, as with the repository's other native live fixtures.

The [fixture directory](../../../tests/fixtures/relational_postgresql/) contains
native DDL, seed/reference rows, typed input, placement and external partition
assignments, a mapping, and an independent hand-checked distance oracle. It runs:

1. Read-only attachment of populated tables, using a SELECT-only login with
   PostgreSQL TEMP revoked. A full database dump must be identical afterwards.
2. Native bulk import into pre-created empty tables with compatible remote
   reference rows. Native PKs, properties, FK values, junction multiplicity,
   reference rows and the unrelated sentinel are compared exactly; schema DDL
   must be unchanged. Native artifact and live verification run as well.
3. Rich, minimal and bounded streamed adjacency, plus BFS with and without
   predecessors, over the PostgreSQL-only graph and the same graph distributed
   across real PostgreSQL → Neo4j → ArangoDB → PostgreSQL stores. Twelve literal
   oracle cases cover directions, filters, depth, isolation and NULL FKs. External
   partitions include a PostgreSQL-to-PostgreSQL cut and cross-backend cuts.
4. Native FK violations, missing remote references, unknown mappings/filters and
   catalog-generation mismatches must fail explicitly.

The service-free runner checks are `python.ingestion_relational_live`. Granular
PostgreSQL suites additionally cover rollback, recovery, stale rows, hostile
identifiers, both FK owner directions, large streams and concurrency. See the
[acceptance matrix](../../architecture/postgresql-relational-acceptance.md).

## Attach an existing database

Use standard libpq environment/credential management. The role needs schema USAGE
and table-level SELECT on every mapped table, including reference/relationship
tables. Runtime and attachment need neither application write access nor TEMP.

```sh
PGHOST=127.0.0.1 PGPORT=5432 PGDATABASE=application PGUSER=graph_reader \
  build/cpu-tests/bin/polygraph_attach_postgres \
  --schema docs/examples/relational-postgresql/schema.json \
  --mapping docs/examples/relational-postgresql/mapping.json \
  --output-dir build/application-identities/generation-1
```

Bootstrap deterministically allocates canonical IDs from typed native keys in a
disk-backed catalog. To join an already prepared mixed-backend graph, also pass
`--source-index prepared/manifest.json`; matching keys retain their canonical IDs
and remote owners. Without this seed, a fresh bootstrap starts a new ID universe.

After controlled application changes, create a new generation with
`--previous build/application-identities/generation-1/manifest.json` and a new
output directory. Surviving IDs stay stable, new IDs append and removed IDs retire;
retired keys cannot silently reappear. Keep active catalogs immutable. There is
no automatic CDC or whole-BFS database snapshot.

Configure a PostgreSQL backend with `relationalMapping: "mapping.json"` and
`relationalIdentity: "generation-1/manifest.json"` on the backend object, alongside
its connection. Relative paths resolve against the backend file. Runtime graph
metadata and `idRouting` must agree with the prepared graph. The existing
`--start-source` / `--source-index` BFS options accept typed external roots. See
the [runtime configuration and traversal contract](../../architecture/postgresql-relational-bfs.md).

## Import into pre-created native tables

Use the `source-jsonl` adapter with explicit typed source identities and both
`--relational-schema` and `--relational-mapping` during `prepare`. PostgreSQL
targets additionally specify `relationalMapping` and a new
`relationalIdentityOutput`. Run normal strict artifact verification, `execute
--confirm-create`, then `verify-live`. The complete input/placement/partition
files used by the live runner are in the fixture directory above.

The importer needs schema USAGE, table SELECT and INSERT, UPDATE on newly imported
FK owners, and PostgreSQL TEMP for private COPY stages. Native constraints and
triggers remain enabled. It appends into empty mapped destinations, tolerates
unrelated populated tables, and never upserts existing owned rows. Remote
endpoints require exact compatible existing rows in reference-only mapped
tables; it never creates incomplete dummy rows. Mixed owned/reference rows in one
destination table are unsupported in this initial policy.

Node, junction and FK writes form one PostgreSQL transaction. Nullable FK cycles
and self-loops work through post-insert assignment; nonnullable type cycles are
rejected. A single FK column can encode at most one distinct target per owner;
conflicting source assignments fail. Native junction rows preserve parallel edges
and optional attributes. See [node ingestion](../../architecture/postgresql-relational-ingestion.md),
[junction ingestion](../../architecture/postgresql-relational-relationship-ingestion.md)
and [FK ingestion](../../architecture/postgresql-relational-fk-ingestion.md).

If PostgreSQL commits but catalog publication is interrupted, inspect the durable
publication journal and run `polygraph_bulk_import recover-relational --manifest
prepared/manifest.json --target-config targets.json`. Recovery verifies committed
rows and publishes the catalog without replaying writes. Do not delete journals,
retry uncertain distributed imports or overwrite existing generations.

## Supported scope and compatibility

Mappings support scalar smallint/integer/bigint, text/varchar and UUID keys.
Namespace, logical type and exact key type distinguish overlapping values.
Composite keys, unsupported SQL domains/enums/arrays/numeric codecs and mapped
RLS are rejected with diagnostics rather than coerced. The native mapping docs
define supported property types and precision. Runtime rich results do not invent
canonical edge IDs from native relationship PKs.

Omit relational configuration to retain generic `polygraph_nodes` /
`polygraph_edges` behavior. The default storage Compose initialization remains
generic. Relational mode never migrates those tables or adds optional traversal
indexes. Parent #12 remains open for explicit acceptance of the documented limits.
