# Dedicated relational relationship tables (#16)

`RelationalRelationshipTableReader` reads mapped PostgreSQL junction tables using
[relational mappings](postgresql-relational-mapping.md) and a published
[vertex identity catalog](postgresql-relational-identities.md). It resolves both
endpoints, reads optional attributes from typed SQL columns, and supports
outgoing, incoming and both-direction lookup. It does not implement node property
reads (#14), direct FK adjacency (#17), relationship ingestion (#19) or runtime
BFS wiring (#21).

## API

The public header is
`data_access/adapters/PostgreSQL/RelationalRelationshipTableReader.hpp`, linked
through `polygraph::backends`:

```cpp
polygraph::postgres::RelationalRelationshipTableReader reader(mapping, identities);
reader.stream(connection, frontier, lookup, consume_batch);
```

Keep the identity resolver alive for the reader's lifetime and its catalog files
immutable. The mapping must match the catalog contract and use the same runtime
node type IDs. `connection` is an idle, caller-owned `PGconn*`; calls must be
serialized by the owner. Callbacks must not reuse the connection or reenter the
reader. The reader neither opens a new database connection nor modifies
application rows/schema.

`RelationalRelationshipLookup` contains:

- `relationship_types`: empty selects every mapped dedicated table. Explicit
  unknown or foreign-key relationship IDs are rejected; direct FK mappings are
  outside this reader's scope.
- `target_types`: empty selects every neighbor type; otherwise filters the
  neighbor type for the selected direction. Unknown node type IDs fail.
- `direction`: `outgoing`, `incoming` or `both`. These are explicit lookup
  directions; they do not reinterpret `Direction::Undirected` as incoming-only.
- `batch_size`: number of physical rows fetched per cursor request, 1–1024.

The frontier contains at most 4096 `NodeRef`s and is resolved through the catalog's
bounded reverse lookup. Repeated frontier IDs are processed once; invalid IDs or
mismatched types fail. An empty frontier emits no batches.

Each `RelationalRelationshipRow` contains the frontier `origin`, logical edge
`type`, resolved `from` and `to` nodes with authoritative owners/source keys, a
`reverse` flag, optional native `external_key`, and JSON `properties`. Stored
orientation stays unchanged in incoming results: `origin` is then `to`, and
`reverse` is true. The neighbor is `to` for a forward row and `from` for a reverse
row. Parallel physical rows are preserved, including identical keyless rows.
In both mode each physical self-loop is returned once per origin; incoming-only
still returns it. An edge connecting two frontier vertices may match both origins.

The optional external edge key is the mapped application PK, not a canonical
`EdgeRef` ID. No hashing or new canonical edge allocation is performed. Keyless
junction tables remain valid; edge identity/import is a later responsibility.

## Native attributes

Only configured columns are projected. SQL NULL remains JSON null; strings are
UTF-8; bigint values remain exact signed 64-bit integers; booleans and finite SQL
real/double values retain their JSON types. Floating output precision is set
transaction-locally to preserve round-trip values. Nonfinite floating values
(NaN/infinity) fail explicitly because JSON cannot represent them as numbers.

Dates use PostgreSQL ISO output strings. Timestamps replace the date/time space
with `T`; timestamp-with-time-zone values are rendered in UTC with a `Z` suffix
for ordinary values, while naive timestamps have no invented zone. PostgreSQL
special date/timestamp spellings remain strings. No precision is intentionally
truncated. Unsupported SQL/logical type combinations fail metadata validation.
Each returned field is limited to 1 MiB; oversized fields fail rather than being
truncated.

## Query and transaction behavior

The reader uses one native-typed JSON frontier relation and a cursor per selected
relationship/direction, rather than one PostgreSQL query per vertex. Native keys
are bound as data; all identifier components are quoted separately. Catalog and
query operators/casts are qualified to prevent application `search_path` shadowing.
Node tables are joined using the referenced PK's explicitly qualified native
collation, so a junction column's collation cannot change identity matching. A
separate bounded join validates every live frontier vertex, including isolated
vertices. Exact stored PK values must still match the catalog's typed source
identity; a case-only PK update is a stale identity requiring refresh. Returned
endpoint keys are resolved through the immutable identity catalog in bounded batches. Missing
live rows, matching NULL endpoints, or identities added after catalog publication
fail explicitly instead of silently dropping relationships or allocating IDs.
Unmatched rows outside the requested frontier are not a whole-database integrity
scan.

A shared RAII snapshot helper is also used by attachment. It acquires ACCESS SHARE
locks on every mapped table before any snapshot-taking SELECT, then validates
metadata and reads in a repeatable-read, read-only transaction. These locks allow
ordinary DML and prevent TRUNCATE/DDL races during the read. PostgreSQL requires
table-level SELECT for the explicit locks; column-only SELECT grants are
insufficient, even though the metadata validator itself supports column-level
privilege checks. No INSERT/UPDATE/DELETE/DDL privileges are needed.

UTF-8 client encoding, ISO DateStyle, UTC TimeZone and floating output precision
are local to the transaction. Commit, query errors and consumer exceptions restore
caller settings. Caller-owned transactions are rejected without committing or
rolling them back. A stream can deliver a prefix before a later error; consumers
must discard that prefix when the call fails. Result order is unspecified.

This is a snapshot of current relationship rows joined to a fixed identity
generation. Out-of-band graph changes still require explicit catalog refresh;
there is no CDC or automatic reconciliation. Owner metadata is preserved, but
cross-backend BFS execution remains #21.

## Tests

```sh
cmake --build --preset cpu-tests --target polygraph_postgres_relational_relationship_test
POLYGRAPH_RELATIONAL_TEST_DSN='host=127.0.0.1 port=55413 dbname=postgres user=akos' \
  ctest --test-dir build/cpu-tests -R '^integration.postgres-relational-relationships$' --output-on-failure
```

Use the owned disposable PostgreSQL test environment. Fixtures create and remove
their own application schemas and reader roles. Missing DSN reports skip code 77;
an explicit DSN that fails to connect fails the test. Existing identity attachment
and metadata tests remain part of regression verification.

The collation regressions require PostgreSQL built with ICU support. They cover
both case-sensitive PKs with case-insensitive junction columns and the inverse.
