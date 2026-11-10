# PostgreSQL relational node ingestion (#18)

The native bulk pipeline can prepare, import and verify nodes in pre-created
application tables using the [relational mapping](postgresql-relational-mapping.md).
Each mapped logical type has its own table and native scalar PK. No generic
`polygraph_nodes` table, application schema migration or synthetic proxy row is
created. Relationship writes remain #19/#20; relational BFS wiring remains #21.

## Input and preparation

Use the built-in `source-jsonl` adapter or a linked adapter with the existing
`typed-external-v1` policy. Canonical-ID JSONL and LDBC adapters retain their
existing generic storage behavior. `source-jsonl` requires explicit record kinds
and structured identities with exactly `namespace`, `type`, `idType` and `id`:

```json
{"kind":"node","type":"Person","identity":{"namespace":"app","type":"Person","idType":"int64","id":7},"properties":{"name":"Zoë","active":true}}
```

Positive signed IDs must say `int64`; they must not become unsigned JSON IDs.
Text PKs use `string`. UUID PKs use canonical lowercase hyphenated UUID strings
with `idType: "string"`. Input namespace and logical type must exactly match the
mapping. Application PKs are preserved separately from canonical routing IDs.
Properties use JSON strings, exact signed integers, booleans, finite numbers or
NULL; date/time properties use native PostgreSQL input strings. No coercion of
strings to numbers or flattening of structured properties is performed.

The adapter also accepts `relationship` records with an `identity` and structured
`from`/`to` identities for generic destinations. This node-only relational writer
explicitly rejects PostgreSQL relationship replicas and proxy rows. Metadata
records, if present, precede data. Source errors include file/line context.

```sh
build/cpu-tests/bin/polygraph_bulk_import prepare \
  --adapter source-jsonl --input nodes.jsonl \
  --placement-config placement.json --output-dir build/relational-import \
  --relational-schema schema.json --relational-mapping mapping.json

build/cpu-tests/bin/polygraph_bulk_import verify \
  --manifest build/relational-import/manifest.json --strict
```

Both relational flags are required together. Preparation is offline and freezes
the logical schema, mapping and mapping fingerprint into the generation. Mapping
changes invalidate preparation checkpoints and cannot be substituted at execution.
In a domain manifest use `ingestion.adapter: "source-jsonl"` and
`ingestion.options.relational-mapping: "mapping.json"`; the existing `paths.schema`
supplies the logical schema. Paths resolve against that manifest's directory.

Artifacts are mapping-bound JSONL, with physical columns and typed scalar values
separate from the canonical ID. PostgreSQL-owned nodes are selected through the
existing normalized placement index. Other owners, partition assignments and
proxy/edge metadata remain in that immutable prepared index; partition fields are
written as application columns only if explicitly mapped. Preparation still
supports other generic backend artifacts in the same generation.

## Destination and write policy

Create the database and application tables before execution. Use an explicit
isolated named database; the existing default/system database safeguards apply.
The target's PostgreSQL entry needs the normal connection fields and:

```json
{
  "targets": {
    "PostgreSQL": {
      "database": "application_import",
      "host": "127.0.0.1",
      "port": 5432,
      "username": "importer",
      "password_env": "APPLICATION_IMPORT_PASSWORD",
      "relationalMapping": "mapping.json",
      "relationalIdentityOutput": "identity-generation"
    }
  }
}
```

Mapping and identity-output paths resolve against the target file. The output
directory must be new, on a filesystem supporting Linux atomic no-replace rename
and directory fsync. Set runtime `relationalIdentity` to its final `manifest.json`
only after successful import or recovery. Generic artifacts and relational targets
cannot be mixed accidentally; mode mismatches fail before writes.

```sh
build/cpu-tests/bin/polygraph_bulk_import execute \
  --manifest build/relational-import/manifest.json \
  --target-config targets.json --confirm-create
```

Initial imports require every mapped node table to be empty. Unrelated populated
tables are allowed and are not modified by the importer. Existing rows are never
merged, updated or deleted. Native SQL constraints and triggers remain enabled;
their own side effects follow PostgreSQL's normal transaction/sequence semantics.
Application schema and optional generic `schema_sql` are never executed here.

Every imported row supplies its external PK. Generated/identity PKs and PKs with
defaults are rejected, avoiding implicit allocation and sequence synchronization.
An omitted property leaves its physical column out of COPY, letting PostgreSQL
apply a default, generated expression or NULL. Explicit JSON NULL stays NULL and
fails for NOT NULL columns. Explicit writes to generated/identity properties fail.
Unmapped required columns without defaults fail preflight. Only mapped columns
are written; unmapped source properties remain in the normalized index.

Preflight validates mapping metadata, SELECT/INSERT privileges, required columns,
varchar character limits and native casts, including smallint/integer overflow.
Scalars are limited to 1 MiB; artifact rows honor `max-record-mb`. Batches normally
contain at most 128 rows or 4 MiB; one larger accepted row is processed alone.
These are buffer controls, not a process RSS guarantee. COPY uses quoted schema,
table and explicit column identifiers and distinguishes NULL, empty strings,
literal `\N`, quotes, newlines and Unicode.

The role needs schema USAGE, table SELECT (including the mapping's relationship
tables), INSERT on written columns and table UPDATE for the write/publication
locks. Active RLS remains unsupported. Import uses SHARE ROW EXCLUSIVE locks;
publication uses SHARE locks. Concurrent writers wait or hit the 15-second lock
timeout. All node-table COPY operations and precommit checks share one transaction;
a constraint or verification failure rolls them back together.

Full verification is mandatory for this mode. It compares native PK membership,
per-table counts and every explicitly supplied column after native casts, then
checks catalog membership and exact canonical IDs. Omitted/defaulted values are
generated by PostgreSQL and are not frozen into the source comparison; subsequent
changes to those values alone are outside this verification contract.

## Catalog publication and recovery

After the application transaction commits, attachment builds an immutable catalog
seeded from the prepared source index, retaining exact canonical IDs and owners.
It writes to a generation-specific hidden staging directory beside the output.
Rows and catalog are checked under locks before atomic publication. Runtime
consumers see no partially built final directory. Once published, the existing
immutable-generation contract applies: external application changes require
explicit refresh and there is no automatic synchronization.

The prepared root contains `relational-postgresql-publication.json`, bound to the
generation, resolved target and mapping. Durable intent precedes commit. A failed
or interrupted publication may leave committed data; never retry COPY into it.
Fix the reported filesystem obstruction, then run:

```sh
build/cpu-tests/bin/polygraph_bulk_import recover-relational \
  --manifest build/relational-import/manifest.json --target-config targets.json

build/cpu-tests/bin/polygraph_bulk_import verify-live \
  --manifest build/relational-import/manifest.json --target-config targets.json
```

Recovery strictly verifies artifacts and current committed rows first. It then
rebuilds its own incomplete staging directory, resumes a completed staged catalog,
or recognizes a rename completed before the final journal write. It refuses
overwriting an unrelated final directory or publishing after explicit source
columns, row membership or mapping have changed. It never reimports application
data. A precommit/uncertain-commit journal is recoverable only when exact stored
rows establish that the prepared import is present. Otherwise provision fresh
isolated destinations; there is no destructive reset or automatic import retry.
The general execution journal can retain a failed import status after recovery;
use `verify-live` to report the recovered generation without repeating `execute`.

## Build and tests

Relational ingestion requires native executors enabled and libpq available.
`polygraph::relational` supplies the shared mapping, catalog and reader code to
both runtime backends and standalone ingestion; runtime does not link ingestion.
The importer also builds with `POLYGRAPH_BUILD_BACKENDS=OFF`.

```sh
POLYGRAPH_RELATIONAL_TEST_DSN='host=127.0.0.1 port=55413 dbname=postgres user=akos' \
  ctest --test-dir build/cpu-tests -R '^integration.postgres-relational-ingestion$' \
  --output-on-failure
```

The opt-in suite creates/drops uniquely named databases on the supplied disposable
server and needs CREATEDB and CREATEROLE for its restricted-role privilege test.
With the environment unset it returns skip code 77.
