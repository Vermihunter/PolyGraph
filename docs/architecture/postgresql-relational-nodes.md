# Typed relational node reads (#14)

`PostgresRelationalNodeReader` reads mapped native PostgreSQL columns into the
existing `Record`, `Value` and `Node` data model. It fills the typed-node read
prerequisite for relational ingestion (#18), using the same
[mapping](postgresql-relational-mapping.md), [identity catalog](postgresql-relational-identities.md),
and scalar conversion rules as [relationship reads](postgresql-relational-relationships.md).
It neither changes application tables nor enables ingestion or BFS.

## API and result shape

```cpp
#include "data_access/adapters/PostgreSQL/PostgresRelationalNodeReader.hpp"

polygraph::postgres::PostgresRelationalNodeReader reader(mapping, identities);
auto records = reader.read_nodes(connection, requested_node_refs);
```

Link through `polygraph::backends`. Keep the identity resolver and immutable
catalog alive for the reader's lifetime. The reader requires an idle caller-owned
`PGconn*`; calls using the same connection must be serialized by its owner.
The mapping must match the resolver's contract and runtime node type IDs.

Each returned `record_t` has one field, a `Value` containing the existing `Node`:

- `global_id` is the catalog's canonical vertex ID, preserving authoritative
  backend bits; application primary keys are never substituted for it.
- `type` is the current logical type ID and `labels` contains the logical type
  name, rather than the physical SQL schema/table name.
- `properties` contains only configured logical property names projected from
  native columns. No whole-row JSONB conversion or JSONB properties column is
  required. Unmapped columns are not returned.

Results follow request order, including repeated IDs. Duplicate requests produce
independent records/values. At most 4096 nodes may be requested; larger requests
fail. An empty valid request returns an empty vector. Records are published only
when the complete call succeeds; no partial vector escapes on failure.
The API materializes the requested results (not the whole graph), with SQL cursor
fetches bounded to 1024 rows. Memory still scales with requested property values;
there is no fixed process-RSS guarantee.

## Types and identities

Native smallint/integer/bigint become exact signed int64 `Value`s, boolean becomes
bool, real/double becomes finite double, text/varchar becomes UTF-8 string, and
SQL NULL becomes the existing null/monostate value. Dates and timestamps use the
same ISO/UTC string representation as relationship attributes. Naive timestamps
do not gain an invented timezone. Nonfinite floating values and unsupported SQL
column types fail explicitly; they are not silently converted to null/string.
Each field is limited to 1 MiB, consistently with other relational readers.

Integer, text and UUID application PKs resolve through the catalog's reverse
lookup. Mixed types in different SQL schemas share one request. The lookup joins
use each PK's explicit native collation, then compare the actual stored typed
source identity exactly with the catalog. Missing live rows or changed PK spelling
(including a case-only change under case-insensitive collation) require refresh
rather than silently returning a different identity. Unknown IDs, wrong type IDs
and mapping/catalog mismatches fail. An unresolved NodeRef type is resolved by
the catalog before table selection.

## Read-only transaction behavior

Node and relationship readers share pure frontier SQL helpers, mapping checks,
PK-collation metadata and the RAII read snapshot. All mapped tables are locked in
ACCESS SHARE mode before taking a repeatable-read, read-only snapshot. The role
needs schema USAGE and table-level SELECT for these explicit locks; no application
write or schema-change privileges are needed.

UTF-8 encoding, ISO DateStyle, UTC TimeZone and round-trip floating output
precision are transaction-local and restore on success/error. Caller-owned
transactions are rejected without modifying them. Metadata is revalidated under
the snapshot, so missing columns and unsupported type drift fail before node
results are returned. There are no CREATE/ALTER/INSERT/UPDATE/DELETE operations,
optional index creation, key rewrites, schema migrations or generic-table writes.

## Verification

```sh
cmake --build --preset cpu-tests --target polygraph_postgres_relational_node_test
POLYGRAPH_RELATIONAL_TEST_DSN='host=127.0.0.1 port=55413 dbname=postgres user=akos' \
  ctest --test-dir build/cpu-tests -R '^integration.postgres-relational-nodes$' --output-on-failure
```

Use an owned disposable PostgreSQL database. Fixtures create their own schemas
and roles; actual reader calls are exercised under SELECT-only privileges.
The collation case requires ICU support. An unset DSN reports skip code 77, while
an explicitly configured failed connection fails. Existing FK, junction,
attachment and mapping suites verify the shared helpers' compatibility.
