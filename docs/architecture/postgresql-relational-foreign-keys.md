# Direct foreign-key relationships (#17)

`RelationalForeignKeyReader` interprets configured SQL FK columns as graph
relationships. It shares the [relationship-read contract](postgresql-relational-relationships.md),
[identity catalog](postgresql-relational-identities.md), and
[mapping validation](postgresql-relational-mapping.md) with the dedicated-table
reader. It creates no application tables, columns, edge records or indexes.

## Ownership and direction

The logical schema declares an edge's `from` and `to` types. A `foreignKey`
mapping declares which endpoint's table holds the SQL reference:

```json
{"type":"ReplyOf","kind":"foreignKey","owner":"from","column":"reply_to_post_id"}
```

For `Comment -> Post`, this reads `comment.reply_to_post_id` against `post`'s
mapped PK. `outgoing` lookup from a comment finds its referenced post; `incoming`
lookup from a post finds every matching comment. A NULL `reply_to_post_id` means
no `ReplyOf` relationship, in every direction.

The opposite ownership is equally supported:

```json
{"type":"Authors","kind":"foreignKey","owner":"to","column":"author_id"}
```

For `Person -> Post`, the reference lives in `post.author_id`. Outgoing lookup
from a person finds their posts; incoming lookup from a post finds its author.
Physical FK ownership does not reverse logical graph orientation. Same-type
self-references and multiple separately mapped FK columns on one table also work.

## API

```cpp
#include "data_access/adapters/PostgreSQL/RelationalForeignKeyReader.hpp"

polygraph::postgres::RelationalForeignKeyReader reader(mapping, identities);
reader.stream(connection, frontier, lookup, consume_batch);
```

The common `RelationalRelationshipLookup`, `RelationalRelationshipRow` and
consumer alias live in `RelationalRelationshipRead.hpp`; both reader headers
include them. Existing table-reader callers remain source compatible.

An empty `relationship_types` list selects only mapped direct FKs for this reader.
Explicit junction-table or unknown IDs fail. The table reader conversely retains
its table-only behavior. Neighbor type filters, duplicate-frontier handling,
maximal frontier size (4096), cursor batch limit (1024), origin/reverse flags and
both-mode self-loop suppression are identical for both readers.

Rows preserve stored logical orientation and authoritative endpoint IDs/owners.
FK rows have no independent external edge PK and expose empty properties. Mapping
v1 does not assign FK relationship attributes, canonical edge IDs, or synthetic
edge records. The consumer must discard any delivered prefix if the call later
fails, and must not reuse the connection from a callback. Batch spans are valid
only during the callback; copy rows that must outlive it.

## Validation and read-only behavior

A mapped FK requires an existing validated, single-column PostgreSQL FK to the
referenced node's complete mapped PK. Metadata/privilege checks occur under the
same locked, read-only repeatable-read snapshot used for junction reads. Native
constraint collation restrictions still apply; PostgreSQL may require identical
collations when nondeterministic equality is involved. The query explicitly uses
the referenced PK's collation and resolves its actual stored key through the
catalog. It does not invent an identity from an alternative reference spelling.

NULL references are omitted before endpoint decoding. Matching non-NULL
references with a missing live target or missing catalog identity fail explicitly.
An FK row that is unrelated to the requested frontier is not a whole-database
integrity scan. Deleted or altered frontier identities fail even when isolated.
After application writes, publish a catalog refresh when new vertex identities
must become visible; no automatic identity allocation or CDC occurs during reads.

As with the table reader, the role needs schema USAGE and table-level SELECT for
snapshot locks. No application INSERT, UPDATE, DELETE or DDL privileges are needed.
The caller supplies an idle connection. UTF-8, DateStyle, TimeZone and floating
precision changes are transaction-local and restore on success or error. Cursor,
rollback, collation and live-membership logic are implemented once in the shared
read engine. No application schema or FK values are changed.

Relational ingestion, FK assignment/cardinality checks and distributed BFS wiring
remain #18–#21. This issue supplies the read path without enabling those workflows.

## Verification

```sh
cmake --build --preset cpu-tests --target polygraph_postgres_relational_foreign_key_test
POLYGRAPH_RELATIONAL_TEST_DSN='host=127.0.0.1 port=55413 dbname=postgres user=akos' \
  ctest --test-dir build/cpu-tests -R '^integration.postgres-relational-foreign-keys$' --output-on-failure
```

Use an owned disposable PostgreSQL database; fixture setup needs DDL and role
creation privileges, while the reader itself is exercised with a SELECT-only role.
ICU support is required for the nondeterministic-collation regression. No DSN
reports skip code 77; a configured unavailable database fails. Junction, metadata
and attachment regressions remain part of the full CPU suite.
