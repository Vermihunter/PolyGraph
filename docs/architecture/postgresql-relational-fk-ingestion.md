# PostgreSQL foreign-key relationship ingestion (#20)

The [relational import pipeline](postgresql-relational-ingestion.md) can represent
relationships in existing node-table FK columns, alongside
[dedicated relationship tables](postgresql-relational-relationship-ingestion.md).
No application columns, tables or constraints are created or disabled. The
[foreign-key reader](postgresql-relational-foreign-keys.md) consumes the same mapping.

## Mapping and cardinality

```json
{
  "type": "FAVORITE",
  "kind": "foreignKey",
  "owner": "from",
  "column": "fav_post"
}
```

The logical schema supplies `from` and `to` types. With `owner: "from"`, the
from-node table holds the FK to the to-node PK. With `owner: "to"`, the to-node
table holds the FK to the from-node PK; logical direction is unchanged. Metadata
validation requires a declared compatible single-column FK to the mapped PK.

Use the existing typed source identity and endpoint format in `source-jsonl` or a
linked typed adapter. Referenced values come from mapped native keys, never
canonical routing IDs. Each owner row and configured FK column can represent at
most one target. Repeated assignments to the same target are idempotent; different
targets for the same owner/column fail offline preparation. One physical FK column
cannot be mapped to multiple logical relationships. Junction tables remain the
representation for multiple targets per owner. Canonical source edge identities
and repeated requests remain distinct in metadata even when native FK state is
one assignment; FK traversal returns that native relationship once.

FK mappings have no native edge PK or attribute mapping. Source relationship
properties, partitions and edge IDs remain in the immutable normalized metadata;
they are not invented as application edge records.

## NULL, defaults and source properties

No source edge for an imported owner means SQL NULL in that FK column. A native
default does not create an implicit graph relationship. A missing assignment to a
NOT NULL FK fails preflight even if the column has a default. A source edge must
have valid non-NULL endpoint selectors; NULL endpoints are not synthetic vertices.

If a node property explicitly maps to the same physical FK column, its value must
agree with the relationship assignment, including NULL. Conflicting properties
fail preparation. Unsupported generated/identity write columns and scalar native
type/range failures follow the normal relational import checks.

## Insertion and assignment

Prepare a fresh generation with the same schema/mapping flags or domain-manifest
options used for relational node/table imports. Offline preparation aggregates
assignments in disk-backed SQLite TEMP state without changing the normalized
index. Node artifacts contain final expected FK values; separate FK artifacts
record owner keys, target keys or NULL and canonical identities. Native node
artifacts are grouped by logical type so dependency-ordered insertion reads each
type's files directly. Generic and table-only workflows retain their existing
representation.

Preflight checks live metadata, native casts, required values, privileges and
insertion feasibility before any backend writes:

- Nullable FK columns are initially inserted as NULL. After all node types exist,
  bounded typed staging batches update owner rows by their native PKs. Exact
  RETURNING checks detect missing owners and source-changing triggers. Statistics
  are refreshed on the private staging table so batch planning can use owner PK
  lookups instead of a generic staging cardinality estimate.
- Nonnullable FKs are supplied during node insertion. Referenced imported types
  are inserted before owner types in a topological dependency order.
- Nonnullable dependency cycles, including nonnullable self references, are
  explicitly unsupported and fail before writes, even for deferrable constraints.
  Nullable cycles and nullable self-loops work through the assignment phase.

The import role needs normal SELECT/INSERT and table UPDATE lock privileges, plus
database TEMPORARY access for assignment staging. Constraints and triggers remain
enabled. Existing native unmapped constraints still apply and can reject the
transaction. Deferred checks/triggers are forced before final verification;
there are no application writes between that verification and commit.

## Ownership and transactions

Only newly imported PostgreSQL-owned owner rows may receive FK assignments.
Pre-existing reference owners are rejected during preparation and never updated.
A remote target can use a compatible existing local reference row under the
relationship-table reference policy: exact keys, reference-only table, authoritative
remote ownership retained in the catalog. Missing local references fail preflight;
the importer does not fabricate proxy rows. Mixing pre-existing reference rows and
new owned rows in one physical table remains unsupported.

Nodes, nullable FK updates and junction-table inserts share one PostgreSQL
transaction. A failed FK update or later relationship insert rolls back all of
them. Final/live verification checks final node values and every owner/FK state,
including owners with no edge. Logical replicated-edge counts are separate from
physical junction-row counts, so FK-only imports and mixed imports verify correctly.

The published normalized index retains canonical edge IDs, replicas, partitions
and proxy ownership. FK expected values remain bound to prepared artifacts;
dedicated tables additionally use their realized-value multiset snapshot.
`recover-relational` verifies both representations and current FK values before
completing catalog publication, and never repeats application writes.

## Verification

The existing `integration.postgres-relational-ingestion` suite includes FK imports.
Set `POLYGRAPH_RELATIONAL_TEST_DSN` to an owned disposable PostgreSQL server with
CREATEDB/CREATEROLE privileges. Cases cover both owner directions, NULL/defaults,
duplicate/conflicting assignments, nonnullable order/cycles, nullable cycles and
self-loops, remote targets/owners, native references, bounded updates, mixed
table/FK imports, rollback and publication recovery. Runtime BFS integration
remains #21.
