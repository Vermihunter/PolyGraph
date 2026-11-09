# Relational PostgreSQL identities (#15)

Relational identity catalogs connect existing application primary keys to
PolyGraph's canonical `GlobalId` values without changing application tables.
They build on the [mapping contract](postgresql-relational-mapping.md).
Relational property reads, adjacency and ingestion remain #14/#16–#21.

## Attach existing rows

Build `polygraph_attach_postgres` with the existing runtime/CPU presets. The
command reuses the runtime's logical schema reader and standard libpq connection
environment. It opens an explicit repeatable-read, read-only transaction; its
role needs schema USAGE and SELECT on mapped columns, not application write or
DDL privileges. It sets UTF-8 client encoding transaction-locally and restores the
caller's encoding on commit/rollback. Before taking its snapshot it acquires
`ACCESS SHARE` locks on all mapped tables, preserving ordinary concurrent DML
while blocking TRUNCATE and incompatible DDL until the scan finishes.

```sh
cmake --build --preset cpu-tests --target polygraph_attach_postgres
PGHOST=127.0.0.1 PGPORT=5432 PGDATABASE=application PGUSER=graph_reader \
  build/cpu-tests/bin/polygraph_attach_postgres \
  --schema docs/examples/relational-postgresql/schema.json \
  --mapping docs/examples/relational-postgresql/mapping.json \
  --output-dir build/relational-identities/generation-1
```

Use `PGPASSFILE` or standard libpq credentials management. Credentials and
connection strings are not written to the artifacts. Output is JSON containing
the new manifest path. The output directory must not already exist.

Attachment validates the mapping inside its snapshot, scans only mapped PK
columns with a cursor (default 4096 rows per fetch), and sorts encoded identities
in a disk-backed SQLite staging table. It does not read application attributes
or materialize the complete key dictionary in memory. A key larger than 1 MiB
is rejected. Its SQLite page cache is 8 MiB; this is not a process RSS hard cap.

Bootstrap assigns monotonically increasing 48-bit payloads in binary order of
`domain::encode_source_identity` keys and encodes PostgreSQL ownership using
`BackendEncodedId::from_local_id`. Mapping order, table scan order and cursor
batch size do not influence IDs. Integer keys become exact signed int64 values;
text/varchar keys preserve exact UTF-8 values; UUIDs use lower-case canonical strings.
Lookup accepts canonical 36-character UUID syntax, with either hex case. The
underlying PostgreSQL UUID parser may have accepted other spellings at insertion;
its stored output is canonical. Namespace and logical type remain part of every
identity. This issue supports scalar integer/text/UUID keys; composite keys are
still rejected.

A fresh bootstrap belongs to a new graph identity universe. It cannot infer IDs
already allocated in a different graph. To attach rows to an existing typed
ingestion graph, adopt that prepared index explicitly:

```sh
build/cpu-tests/bin/polygraph_attach_postgres \
  --schema schema.json --mapping mapping.json \
  --source-index prepared/manifest.json \
  --output-dir identities/generation-1
```

Adoption verifies the prepared index checksum, preserves exact matching source
IDs and authoritative owners (including Neo4j/ArangoDB), and reserves payloads
for every prepared entity, including relationships and unmapped node types.
Relational catalogs must use `--previous`; they cannot be passed as ingestion
seeds to discard retirement history. Only nodes matching the mapping's logical
type, namespace and key codec are
adopted. A UUID seed must already use canonical source-key spelling. New keys
receive PostgreSQL-owned IDs after the reserved allocation watermark. Adoption
does not create proxies, choose partitions, move data between backends, or prove
that remote-owned SQL rows are actual graph replicas. Those remain placement and
adjacency responsibilities. Later relational ingestion must consume this catalog
rather than independently allocating replacement IDs.

## Immutable generations and refresh

```sh
build/cpu-tests/bin/polygraph_attach_postgres \
  --schema schema.json --mapping mapping.json \
  --previous identities/generation-1/manifest.json \
  --output-dir identities/generation-2
```

Refresh verifies and copies the previous catalog, keeps surviving IDs and owners,
and appends IDs for new keys in deterministic order. Removed keys become retained
tombstones and disappear from active lookup. Their IDs are never recycled.
A retired application key reappearing in a later generation is an error requiring
an explicit identity migration, because attachment cannot tell key reuse from
resurrection. Existing generations remain readable and unchanged.

The manifest identifies `polygraph.relational-identity` version 1, the prepared
`typed-external-v1` source-root contract, mapping fingerprint, allocation history,
active node count, and index size/SHA-256. Generation ID is the catalog SHA-256.
The fingerprint binds mapped logical type names and property types, namespaces,
codecs, physical tables/columns and relationship endpoint mappings; it ignores
mapping-array order and runtime-local numeric type IDs. Changed mapping contracts
require migration instead of silent refresh. This is not a fingerprint of unmapped
logical schema entries or application row values.

The SQLite index and manifest are fsynced; the manifest is published last. Failed
attachment removes only its newly created output directory, leaves the source
catalog unchanged, and rolls back any open PostgreSQL snapshot. A process crash
may leave an incomplete output directory; it is not a valid published generation
and must not be reused automatically. Keep published files immutable for the
entire lifetime of runtimes using them. There is no CDC or live synchronization;
application writes after a snapshot require a new generation.

## Resolver API

The backend library links SQLite and OpenSSL Crypto, both existing project
dependencies. It does not link the ingestion engine. Public interfaces live in
`RelationalIdentityResolver.hpp` and `RelationalAttachment.hpp` under
`data_access/adapters/PostgreSQL/`.

```cpp
polygraph::postgres::RelationalIdentityResolver resolver(manifest, mapping);
auto nodes = resolver.resolve(source_identity_batch);
auto sources = resolver.reverse(node_ref_batch);
```

Each result contains canonical `NodeRef`, owner and normalized `SourceIdentity`.
Lookups preserve input order, reject more than 4096 inputs, and use per-call
read-only SQLite handles with indexed point queries. They reject unknown or
retired keys/IDs, mismatched namespaces/types and noncanonical owner/ID metadata.
A constructor can additionally require an exact expected generation string.
Checksums, path confinement and the mapping fingerprint are verified at open;
files must remain immutable afterwards. Existing `cli::SourceNodeIdResolver`
can read the catalog's active `nodes` view, so `--start-source`/`--source-index`
reuse the prepared source-root contract. Backend adjacency is not implemented by
this issue; configuring relational traversal still fails explicitly until #21.

The C++ attachment API requires an idle caller-owned `PGconn*` and returns the
published manifest path. It refuses a caller-owned transaction rather than
committing or rolling back work it did not start. Failed allocation or key reuse
never modifies application tables.

## Verification

```sh
ctest --test-dir build/cpu-tests -R 'unit.RelationalIdentity' --output-on-failure
POLYGRAPH_RELATIONAL_TEST_DSN='host=127.0.0.1 port=55413 dbname=postgres user=akos' \
  ctest --test-dir build/cpu-tests -R '^integration.postgres-relational-identity$' --output-on-failure
```

Live tests use an owned disposable PostgreSQL instance and exercise actual
attachment, refresh, reverse lookup, runtime source roots, prepared-ingestion
adoption, command-line attachment and a SELECT-only role. They create temporary
application schemas and clean them up; the test role requires DDL/role-creation
privileges for fixture setup. Unset test DSN reports skip code 77; an explicitly
configured unavailable database fails.
