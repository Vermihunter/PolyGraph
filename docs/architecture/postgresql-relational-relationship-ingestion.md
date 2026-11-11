# PostgreSQL relationship-table ingestion (#19)

The [relational import pipeline](postgresql-relational-ingestion.md) writes edges
into configured `kind: "table"` relationship tables after their nodes exist.
The same mapping drives the [relationship-table reader](postgresql-relational-relationships.md).
[Foreign-key assignment writes (#20)](postgresql-relational-fk-ingestion.md) use
the same transaction; the [runtime adapter (#21)](postgresql-relational-bfs.md)
combines both representations for BFS.

## Mapping and source identities

Use the existing node mappings plus a dedicated relationship mapping:

```json
{
  "type": "LIKES",
  "kind": "table",
  "schema": "app",
  "table": "likes",
  "fromColumn": "person_id",
  "toColumn": "post_id",
  "primaryKey": "id",
  "keyCodec": "int64",
  "properties": {"weight": "weight", "note": "note"}
}
```

`primaryKey` and `keyCodec` are optional together. With them, an edge's structured
source identity supplies its exact native PK. Integer, text and UUID codecs follow
the node import rules. The relationship mapping has no namespace field: the source
identity retains its namespace in metadata, while its value supplies the native
edge key. Distinct source namespaces cannot evade native PK uniqueness.

Without them, every input edge is a separate physical row, including parallel
identical rows and self-loops. Canonical edge IDs remain in metadata; the importer
does not add application columns or derive canonical IDs from endpoint pairs.
A PK may also be an endpoint/property column when their exact values agree.

`source-jsonl` accepts relationships with structured endpoint selectors:

```json
{"kind":"relationship","type":"LIKES","identity":{"namespace":"app","type":"LIKES","idType":"int64","id":91},"from":{"namespace":"app","type":"Person","idType":"int64","id":7},"to":{"namespace":"app","type":"Post","idType":"string","id":"00000000-0000-0000-0000-00000000000a"},"properties":{"weight":1,"note":null}}
```

Endpoint selectors must resolve to normalized source vertices of the mapping's
logical endpoint types. The writer extracts each vertex's canonical native key;
internal routing IDs are never written into endpoint columns. Wrong/missing
types and unresolved source identities fail offline preparation. Explicit edge
identities are required for mapped edge PKs;
keyless tables also support normalized edges without an external edge identity.

## Import, native values and verification

Use the same prepare/import/verify/recovery CLI commands and target fields as node
ingestion. No extra execution switch or application DDL is needed. Mapped node
write destinations and dedicated relationship tables must be empty; unrelated
populated tables remain allowed. All node and relationship copies share one
transaction. Native constraints and triggers remain enabled; a later edge failure
rolls back every node and edge written in that transaction.

Preflight checks native casts, required values, INSERT and lock permissions before
any backend starts. Relationship imports also require database TEMPORARY access
for private typed staging tables; this privilege is checked during preflight.
Explicit writes to generated/identity columns and automatic mapped PKs follow the
same rejection policy as node imports. Unmapped generated/default columns remain
PostgreSQL's responsibility.

Mapped attributes retain native scalar types. Omitted attributes use native
defaults or NULL; explicit JSON NULL remains NULL. COPY batches use explicit
columns, with separate handling of NULL, empty strings, literal `\N`, multiline
text and Unicode. Each batch is copied into private typed staging columns, then
inserted into its mapped table with `INSERT ... RETURNING`. Returned rows are
checked against source-supplied native values without rescanning earlier batches.
Triggers that silently rewrite those values cause rollback.

Each verified inserted batch also captures its realized mapped values, including
defaults. Deferred constraints and triggers are forced before final verification,
while rollback remains possible. Before commit, verification compares the complete stored relationship
multiset against that snapshot using bounded PostgreSQL cursor reads and a
disk-backed SQLite counter. Counts preserve identical parallel rows across batch
boundaries; this is not a distinct-row/set comparison. Later verification and
recovery reuse the checksummed snapshot, detecting changed defaults as well as
explicit attributes, membership and multiplicity. Scalars remain limited to 1 MiB
and realized snapshot rows must fit the prepared `max-record-mb` limit. Buffers
are bounded by batches; no hard process RSS limit is claimed.

## Placement, partitions and proxy references

The existing normalization rules still choose owners, relationship replicas,
partition-cut metadata and proxies. PostgreSQL imports its replica set, even when
an edge's authoritative owner is another backend. Backend counts retain separate
authoritative-node, proxy and replicated-edge counts.

A proxy endpoint requires an already existing compatible local application row
with the exact mapped PK. Reference-only tables must contain exactly the required
reference keys; those rows are read and preserved. Missing remote-only rows fail
preflight. The importer never fabricates incomplete application rows. Mixing new
owned vertices and pre-existing reference vertices in the same physical table is
unsupported under the initial empty-write-destination policy. A reference's
canonical ID and authoritative owner are seeded unchanged into the node catalog.

The published identity directory additionally contains `ingestion.json` and a
checksummed `ingestion.sqlite3` copy of the immutable normalized index. Its
`source_edges`, `edges`, `replicas`, `proxies` and partition records retain exact
canonical edge identities and placement independently of physical application
columns. `relationship-rows.jsonl` retains the native verification snapshot.
`ingestion.json` binds these files to the source generation and import target.
Verification checks these side files before accepting the catalog. Physically
identical keyless rows remain indistinguishable; their source edge identities
remain separate metadata records.

## Publication and recovery

The existing durable publication journal now also binds the relationship snapshot.
Its intent is written before application commit. After commit, the staged node
catalog and side metadata are verified under SHARE locks, then published with the
same atomic no-overwrite directory rename. Failed publication can be completed
with `recover-relational`; it verifies nodes, references, exact edge values and
metadata checksums first and never repeats COPY. Corrupt/missing snapshots or
changed committed rows fail explicitly.

The live `integration.postgres-relational-ingestion` suite covers both node and
relationship imports, using uniquely named disposable databases. It needs
`POLYGRAPH_RELATIONAL_TEST_DSN`, CREATEDB and CREATEROLE. Cross-backend reference
tests use real PostgreSQL reference rows and retained backend metadata; they do
not claim live Neo4j/ArangoDB execution or distributed BFS acceptance.
