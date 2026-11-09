# PostgreSQL relational schema mappings (#13)

Issue #13 introduces the mapping contract and a read-only PostgreSQL metadata
validator. It does not implement relational node reads, relationship ingestion, or BFS. Those follow in #14–#21.
Issue #15 adds [identity catalogs and attachment](postgresql-relational-identities.md). Existing generic
`polygraph_nodes` / `polygraph_edges` workflows remain the default.

See the matching [mapping example](../examples/relational-postgresql/mapping.json),
[logical schema](../examples/relational-postgresql/schema.json), and illustrative
[application DDL](../examples/relational-postgresql/schema.sql).

## Mapping format

A file contains `format: "polygraph.postgresql-relational"`, integer `version: 1`,
a nonempty `nodes` array, and a `relationships` array (possibly empty). Unknown
fields are errors. Logical types refer to `LogicalGraphSchema`, while schema,
table, and column names are exact PostgreSQL identifiers, not SQL expressions.
Identifiers must be nonempty, NUL-free and at most 63 UTF-8 bytes, matching the
standard PostgreSQL identifier limit. Mixed case, dots, quotes, spaces and Unicode
are preserved; schema and table are separate fields.

A node entry requires `type`, `schema`, `table`, `primaryKey`, `namespace`, and
`keyCodec`. Optional `properties` maps logical property names to column names.
The namespace is the source-identity namespace; it does not allocate IDs or rename
application keys. Primary keys are scalar:

| keyCodec | Supported native key types | Future source identity representation |
| --- | --- | --- |
| `int64` | smallint, integer, bigint | signed int64 |
| `string` | text, varchar | exact string |
| `uuid` | uuid | normalized UUID string (normalization belongs to #15) |

Arrays of key columns, composite PKs and other codecs fail explicitly. Each node
type and each physical table may be mapped only once. A mapping can describe a
subset of logical types, but every mapped relationship requires both endpoint
node mappings. No shared-table discriminator or arbitrary SQL predicate exists.

Relationship `type` determines `from`/`to` through the logical schema:

- `kind: "table"` requires `schema`, `table`, `fromColumn`, and `toColumn`.
  Endpoint columns must differ. Optional `properties` maps logical attributes.
  Optional `primaryKey` and `keyCodec` must appear together; an edge table without
  its own key is allowed, including one with a composite physical primary key.
  Identity for such edges is deferred to #15/#19. Junction endpoints need not
  declare SQL FKs, but their SQL types must match the endpoint key codec.
- `kind: "foreignKey"` requires `owner: "from" | "to"` and `column`. The owner
  chooses which logical endpoint's node table contains the FK; the other endpoint
  supplies the referenced PK. This does not reverse logical graph direction.
  FK mappings have no table override, relationship PK, or attribute mapping.
  Nullable columns are accepted and their nullability is retained for later
  readers/writers. A validated declared single-column FK to the mapped PK is
  required. Reusing one physical FK column for multiple mappings is rejected.

Property columns must match the logical property type:

| Logical type | Supported SQL types |
| --- | --- |
| string | text, varchar |
| integer | smallint, integer, bigint |
| boolean | boolean |
| float | real, double precision |
| date | date |
| datetime | timestamp without time zone, timestamp with time zone |

This is metadata compatibility, not value conversion. SQL NULL, exact integer,
UUID normalization and timezone conversion rules will be implemented with typed
reads/import. Numeric/decimal, domains, enums, arrays, user-defined types and
implicit string conversion are not supported by this version. Unmapped columns
are left alone, including generated/default columns.

## API and validation

The public headers are `data_access/adapters/PostgreSQL/RelationalMapping.hpp`
and `RelationalMappingValidator.hpp`, linked through `polygraph::backends`.

```cpp
auto mapping = polygraph::postgres::load_relational_mapping(mapping_file, logical_schema);
auto validated = polygraph::postgres::RelationalMappingValidator{}.validate(connection, mapping);
// validated.mapping() and validated.tables() expose immutable metadata.
```

`connection` is a caller-owned open `PGconn*`; the caller owns transaction and
connection synchronization. Parsing and validation throw `std::invalid_argument`
with logical object, physical column or configuration-file context. The validator
issues only parameterized catalog/privilege SELECTs with catalog-qualified types and
operators as well as catalog-qualified functions and relations. It neither reads application
rows nor changes schema, data, permissions or session settings. It works in a
read-only transaction and with SELECT granted only on mapped columns.

Validation checks exact schema/table existence, regular/partitioned table kind,
column existence, scalar node/optional edge PK constraints, key/property types,
FK endpoints/validation, schema USAGE, and mapped-column SELECT privileges.
Traditional inheritance parents are rejected because their PK constraints do not
cover descendant rows; declarative partitioned tables remain supported.
Active row-level security is rejected because a partial visible graph cannot
currently establish complete adjacency coverage. A role bypassing RLS is evaluated
according to that role's effective visibility. Import privileges, defaults,
required-column feasibility, data integrity for unconstrained junctions and
cross-backend references belong to later ingestion/identity issues.

The result retains relation OIDs and column numbers, native type OIDs/modifiers,
nullability, generated/default and identity flags. It is a startup snapshot, not
a schema lock or a promise of row-level integrity. Revalidate after DDL; callers
must use appropriate transaction/snapshot boundaries when consuming the result.

The implementation follows PostgreSQL's documented
[constraint catalog](https://www.postgresql.org/docs/18/catalog-pg-constraint.html),
[column catalog](https://www.postgresql.org/docs/18/catalog-pg-attribute.html), and
[parameterized libpq execution](https://www.postgresql.org/docs/18/libpq-exec.html).
It is exercised against PostgreSQL 18; queries also avoid newer-only catalog
columns where older supported PostgreSQL versions differ.

## Configuration boundary

A PostgreSQL runtime backend accepts a top-level mapping reference:

```json
{
  "id": "PostgreSQL",
  "kind": "PostgreSQL",
  "relationalMapping": "mappings/application.json",
  "connection": {"endpoint": "localhost:5432", "database": "application"}
}
```

The backend file loader resolves the reference against the containing file,
independently of the working directory. Programmatic `parse_connection_config`
callers must supply an absolute reference. `relationalMapping` inside `connection`
or on another backend kind is an error. Omitting the field preserves generic mode.
The factory parses the referenced mapping against the logical schema, then rejects
relational traversal explicitly until #21; it never falls back to generic SQL.
Use the validator API to check metadata in #13. The #15 attachment command also
validates mappings while building an identity catalog.

The native importer explicitly rejects targets carrying `relationalMapping`
until #18. It must not feed generic CSV artifacts into mapped application tables.
Ingestion mapping references and artifact binding will be introduced with that
pipeline rather than implying a working relational import now.

## Tests

```sh
cmake --build --preset cpu-tests --target \
  polygraph_postgres_relational_mapping_test \
  polygraph_postgres_relational_config_test \
  polygraph_postgres_relational_metadata_test
ctest --test-dir build/cpu-tests -R 'unit.Relational' --output-on-failure
```

The live tests require a disposable database and a role able to create schemas
and test roles. Every case wraps its objects and roles in a rolled-back
transaction; do not point them at an application database.

```sh
POLYGRAPH_RELATIONAL_TEST_DSN='host=127.0.0.1 port=55413 dbname=postgres user=akos' \
  ctest --test-dir build/cpu-tests -R '^integration.postgres-relational-metadata$' --output-on-failure
```

Without that explicit variable CTest reports the live test as skipped (code 77).
With it set, failed connections and failed validation assertions fail the test.
