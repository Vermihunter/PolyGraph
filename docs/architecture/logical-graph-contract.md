# Logical graph and input adapter contract

This is the specification for [issue #29](https://github.com/Vermihunter/PolyGraph/issues/29),
under [Phase 1 (#24)](https://github.com/Vermihunter/PolyGraph/issues/24) of
[graph extensibility (#23)](https://github.com/Vermihunter/PolyGraph/issues/23).
It builds on the completed [audit (#28)](graph-extensibility-audit.md), the first
task referred to as “issue 01” in the issue sequence.

**Status:** the existing record/streaming model remains the canonical pipeline.
Issue #30 implements statically linked factory registration, discovery, input
inventory and ID-policy selection; see [registration API](../guides/BULK_INGESTION.md#registering-a-statically-linked-input-adapter).
Issue #31 adds [manifest-based read-only preflight](../guides/GRAPH_MANIFESTS.md)
and native preparation using an additive `polygraph.domain` version 1 extension.
Sections marked **extension contract** specify requirements for later adapter,
manifest and normalization work (#30–#32); they are not new accepted JSONL fields,
CLI options unless explicitly described as implemented. Issue #29 added the
specification and examples; source namespaces and generic ID allocation remain
future work.

## Representation and ownership

Use [ingestion::Record and IGraphInputAdapter](../../include/ingestion/Import.hpp)
for ingestion, and [LogicalGraphSchema](../../include/domain/graph/LogicalGraphSchema.hpp)
for runtime type declarations. A vertex has one named node type and named typed
attributes. An edge has one named edge type, two vertex references and attributes.
Names are case-sensitive. Runtime `TypeId` values are process-local schema handles,
not external IDs or persisted global IDs. Do not embed SNB labels in generic code.

| Information | Owner / existing representation |
| :-- | :-- |
| Source files, parsing, source namespace, source ID interpretation, source-to-logical type/property mapping | Input adapter; source configuration and provenance belong in graph metadata. |
| Vertex/edge type, values and endpoint references | Adapter `Record.type`, `properties`, `key`, `from`, `to`, optional `id`. The called stream method identifies the entity kind. |
| Graph name/version, adapter name/version/options, inventory, schema, identity strategy/version and validation policy | Graph-level configuration/manifest; reuse existing CLI configuration, graph metadata and `domain.json` paths. A unified versioned manifest is #31, not a second format introduced here. |
| Owner selection, partition-to-owner mapping, adjacency replication and proxies | Existing placement and normalization stages. `Record.source` means a **backend**, not a source namespace. |
| Credentials, endpoints, SQL tables, labels/collections, import flags, reserved physical fields | Backend configuration and writers/executors; never source identity. |
| Checksums, generation/checkpoint identity, counts, ID prefix mapping, partition artifacts | Engine-generated native `manifest.json`; do not confuse it with a user graph manifest or runtime `domain.json`. |

### Types and direction

The existing runtime schema syntax is sufficient for scalar declarations:

```json
{
  "nodeTypes": [
    {"name":"Device","properties":[{"name":"external_id","type":"integer"}]},
    {"name":"Site","properties":[{"name":"name","type":"string"}]}
  ],
  "relations": [
    {"name":"INSTALLED_AT","from":"Device","to":"Site","directed":true,
     "properties":[{"name":"since","type":"date"}]}
  ]
}
```

Type names must be unique within their kind, and edge declarations must reference
declared node types. The runtime currently represents one endpoint type pair per
edge name. Native ingestion can retain heterogeneous endpoint pairs; the Docker
runtime bootstrap explicitly rejects them when a BFS schema is required. This
contract does not retroactively reject a native graph that cannot use that bootstrap.
Some schema construction APIs do not enforce all uniqueness rules today; a future
preflight must validate them rather than rely on map overwrite behavior.

Records store an ordered `from`/`to` pair. Direction is a schema property, not a
second edge copy or a new record field. `directed` defaults to `true` in the runtime
loader; `false` describes an undirected logical edge while retaining its stored
endpoint pair. Query `ExpandSpec.direction` independently requests `Directed`
(outgoing), `Undirected` or `Both` (bidirectional); there is no incoming-only
selector. Declaring a direction does not add new query/backend
capabilities; preserve the current BFS direction/filter semantics. Do not invent a
reverse edge on import. Parallel edges and self-loops are valid logical records;
partition CSR deduplication and BFS visited-node deduplication do not delete them.

## Source identity, global identity and collision avoidance

**Extension contract:** a source identity is the structured tuple

```text
(source namespace, entity kind, logical type, source ID type, source ID value)
```

The namespace is a stable graph-scoped dataset/import-source name, independent of
paths, backend assignment and cache residency. A graph combining sources must
assign distinct namespaces unless it explicitly intends shared identity. Endpoint
references use a **vertex** identity tuple, including the referenced type and
namespace. IDs are non-null strings or exact signed/unsigned 64-bit integers.
Integer `7`, unsigned integer `7` and string `"7"` are distinct unless the adapter
declares a conversion. Text IDs preserve spelling, case, whitespace and Unicode;
any trimming, normalization or conversion must be declared by the adapter and
included in input identity. Floats, booleans, objects, arrays and implicit numeric
coercion are not source IDs. Composite source keys need an explicit lossless
adapter mapping; no generic composite-key API is introduced here.

For example, `(inventory, vertex, Device, uint64, 7)` and
`(inventory, vertex, Site, uint64, 7)` are **different vertices even on the same
backend**. `(other, vertex, Device, uint64, 7)` and the corresponding edge identity
are also distinct. Future key encoding must be injective (structured serialization
or length-delimited components), never naive concatenation or an unchecked hash.
An endpoint does not resolve by its numeric value alone.

Today `Record.key/from/to` are opaque strings into the SQLite resolver:

- LDBC uses `Type:raw-id` vertex keys. Its closed type catalog and numeric raw ID
  syntax make that encoding unambiguous. The block31 dictionary keys on
  `(type, raw ID >> 31)`, preserving equal IDs across node types. Prefix codes
  occupy payload bits 31–46; relationships use bit 47 plus type and sequence.
- Canonical JSONL uses the decimal canonical GlobalId as the key and endpoint
  reference. **A JSONL `uuid` is already a global ID**, not an arbitrary source
  ID. Changing `type` cannot disambiguate a reused global ID. Ordinary properties
  such as `external_id` or `ldbc_id` do not participate in JSONL identity.
- Current records have no namespace/source-ID-type fields. Arbitrary typed source
  identities require the additive work in #30–#32. Do not pass the tuple above to
  today's JSONL loader or treat every ID-less record as a supported new adapter:
  shared normalization currently falls back to `LdbcIds`.

### Stable and generated IDs

Source identity is stable if the source promises it. GlobalIds encode a persistent
owner plus a 48-bit payload, with additional location bits for cache residency.
Canonicalization removes residency, not owner or payload. Vertices and edges share
one collision-checked global namespace in native normalization. The same payload
on different owners is not the same global identity. Changing ownership requires
an explicit migration; placement cannot silently rewrite canonical JSONL IDs.

Canonical input IDs are preserved. Generated IDs must be unique, bounds-checked,
and reproducible for the **same input inventory, traversal order, adapter/strategy
version and placement** when determinism is promised. They need not survive input
reordering, inserted records, changed owners or allocator versions. Record the
strategy/version and required mappings; never advertise an ordinal as a stable
external source ID. Source data may carry an edge ID, but it is optional at the
logical adapter boundary. An edge without one remains a distinct occurrence:
assign an implementation ID in deterministic stream order, not a hash of endpoints
or properties that merges parallel edges.

Today SNB has no explicit edge IDs and generates them per type/per owner sequence.
Canonical JSONL requires `uuid` on **both** vertices and edges; omitting an edge
`uuid` is an error, not a request for generic allocation. Its owner must agree
with any explicit `source` and configured placement. Edge ownership is independent
of either endpoint's ownership.

## Values and validation policy

`Record.properties` is a JSON object. Existing schema scalar tags are `string`,
`integer`, `boolean`, `float`, `date`, `datetime`; date/time values remain source
strings in current ingestion, without automatic parsing/timezone conversion.
Booleans must not be reinterpreted as integers. Preserve exact integer values;
do not pass 64-bit IDs through floating-point intermediates. Non-finite floats,
binary values and arbitrary runtime `Value` variants are outside portable JSON
input; an adapter must explicitly encode them (for example as declared text) or
reject them. JSONL does not apply LDBC numeric-column heuristics.

The schema describes values; native `prepare` does not currently read a schema
file or enforce required properties, nullability, undeclared-property rejection
or every declared scalar type. Do not add `nullable`/array type tags to the runtime
schema and assume support. In particular, properties can contain arrays even
though `PropertyType` has no array tag. The example's `tags` array is intentionally
not declared as a fictional runtime array type.

| Case | Contract and current behavior |
| :-- | :-- |
| Absent property / explicit null / empty string | Distinct in normalized JSON; missing is not an inferred default. SNB's adapter omits empty cells. Neo4j stores null as absence; PostgreSQL/Arango preserve explicit JSON null. This backend difference is intentional. |
| Portable properties | Strings without NUL, signed int64, finite floats, booleans, homogeneous scalar arrays, date/time strings; null is portable with the absence distinction above. |
| Backend-dependent values | Objects, nested or mixed/null-containing arrays can be retained by JSONB/Arango. Neo4j-targeted records fail preparation for unsupported values; no automatic lossy stringification. Neo4j unsigned properties beyond int64 also fail; this restriction does not prohibit uint64 GlobalIds represented as signed storage IDs. |
| Repeated vertex source identity or canonical ID | Error, even for identical rows; no implicit merge/upsert/last-wins. Equal IDs in different source types remain distinct before global allocation. |
| Repeated explicit edge identity or global ID | Error. Equal endpoints/properties with distinct IDs or distinct ID-less occurrences are parallel edges, not duplicates. |
| Missing endpoint | Error before publishing a prepared manifest, with endpoint context. Generated cross-backend proxies represent known remote vertices, never repairs for missing source vertices. No skip/create/dangling policy is implemented or selected by default. |
| Unsupported value, malformed ID, exhausted allocator, conflicting source/owner | Error, never truncate, coerce, wrap or silently move the record. Adapters report source file/record context; engine validation adds identity/type/endpoint context where available. |

Duplicate JSON member names are not a mechanism for multiple attributes. Producers
must emit unique names; current JSON parsers may collapse repeated object keys,
so strict duplicate-member detection is not an existing guarantee. Physical
sanitization collisions, reserved `Proxy`/lock names, NUL and partition-property
collisions are rejected by the native engine. Backend metadata keys such as
`uuid`, `localId`, `type`, `source`, `_key`, `_from`, `_to`, `is_proxy` are owned by
writers and may be overwritten or omitted; adapters must map conflicting source
attributes to safe names. Do not infer logical identity from those properties.

## Streaming and extension API boundary

Retain `IGraphInputAdapter::nodes(RecordConsumer)`, `relationships(RecordConsumer)`
and `metadata() const`. Consumers receive records by value. Each pass is streamed;
adapters must not require an all-vertices/all-edges in-memory map. Adapter instances
are called sequentially; no concurrent-callback or shared-state thread-safety
promise is implied. The current engine may create a separate instance to project
partition topology, so replaying immutable inputs must preserve keys and ordering.
Do not generate identities from process-local addresses or randomness.

The normalization order is: read all vertices into the SQLite identity/endpoint
index, then resolve edges. Thus an edge's vertex may appear anywhere in the vertex
pass, and JSONL record interleaving is handled by separate scans. No on-disk source
ordering of “edge after its nodes” is required. Edge-to-edge references and an
unbounded live update stream are not this snapshot contract.

`InputAdapterRegistry` uses **statically linked C++ adapter factories** selected
by stable names, retaining `ldbc-snb` and `polygraph-jsonl`. It is not an ABI
contract: no `dlopen`, runtime dynamic libraries, code generation, scripting
runtime, or new ingestion framework. `src/ingestion/Input.cpp::make_adapter`
delegates to the selected registry, which also provides the input inventory.

| Boundary | Responsibility |
| :-- | :-- |
| Adapter | Parse/map source values and references, enumerate input inventory and provenance, declare identity semantics. No database connections, proxy creation or SQL/Cypher/AQL emission. |
| Identity policy + normalization | Canonical-ID validation or adapter-associated payload allocation **after owner selection**, typed source-key mapping, duplicate detection and endpoint closure in the existing SQLite index. #32 removes the hardwired LDBC fallback. |
| Placement / partitioning | Choose raw owners, preserve canonical owners; generate partition metadata separately from source keys/IDs. Edges keep independent configured ownership. |
| Backend writers and `IImportExecutor` | Serialize normalized graph rows, enforce backend value constraints, import and verify physical storage. All three backends consume the same logical records. |

Adapter metadata can be finalized while streaming (the existing JSONL adapter
learns leading metadata during scans); do not assume it is complete before the
passes. Input inventory is now available separately through registry callbacks;
read-only metadata inspection is an optional registry callback used by preflight.
Final IDs cannot be allocated
before a partition-to-owner map has selected raw vertex ownership.

## Compatibility and migration

1. Preserve current APIs, CLI names, JSONL fields, owner masks, SNB catalog/iteration
   order and `ldbc-block31-v3`/`preserve-canonical-v2` semantics. This contract alone
   does not change native manifest `schema_version`/`engine_version` or invalidate
   existing artifacts. Graph format/version and ID strategy are separate concepts.
2. Static registration is implemented through the existing adapter seam in #30.
   Source namespace/type metadata belongs to the remaining manifest and ID work;
   #31 implements additive graph configuration/preflight in `polygraph.domain`
   version 1. Legacy runtime manifests and native CLI flags remain supported;
   native manifest preparation opts in with an `ingestion` section and native
   paths. Old SNB and canonical JSONL remain accepted without mandatory new record fields.
3. Generalize source-key resolution in #32 while keeping the current SQLite index,
   streaming passes, LDBC allocator and preserved-ID JSONL path. Explicitly version
   any new identity key encoding. Existing single-source adapters use a documented
   legacy namespace; do not retroactively prefix their persisted source keys.
4. Include adapter/options/identity-policy versions and input inventory in generation
   identity before introducing new allocation semantics. Changed identities require
   a fresh generation or explicit migration of nodes, edges, roots, native ID maps,
   proxies and cache artifacts. Never reuse a matching-looking checkpoint or cache
   under different semantics; retain old readers or reject unsupported versions.
5. Preserve schema-directed BFS and backend proxy conventions (PostgreSQL logical
   endpoint types versus Neo4j/Arango `Proxy`). Do not widen schema/cache capabilities
   or change placement strategies as a side effect of this contract.

## Runnable example and verification

The checked-in [schema](../examples/ingestion/logical-graph/schema.json),
[canonical JSONL](../examples/ingestion/logical-graph/graph.jsonl) and
[placement](../examples/ingestion/logical-graph/placement.json) use four node types
with the same integer external ID `7`, including Device and Site on Neo4j. Each
has a distinct canonical ID. Two identical INSTALLED_AT endpoint/property pairs
have distinct edge IDs; their multiplicity is preserved. The Sensor's uint64 ID
uses a decimal string to avoid JSON consumer precision loss. `external_id` is
provenance only; arbitrary typed source-ID ingestion remains future work.

Run from the repository root with a fresh output directory:

```bash
cmake --build --preset cpu-tests --target polygraph_bulk_import
build/cpu-tests/bin/polygraph_bulk_import prepare --adapter polygraph-jsonl \
  --input docs/examples/ingestion/logical-graph/graph.jsonl \
  --placement-config docs/examples/ingestion/logical-graph/placement.json \
  --output-dir build/logical-contract-example
build/cpu-tests/bin/polygraph_bulk_import verify \
  --manifest build/logical-contract-example/manifest.json --strict
ctest --test-dir build/cpu-tests -R python.ingestion_semantics --output-on-failure
```

The schema illustrates the existing runtime declaration format; it is deliberately
separate from native preparation, which takes graph/placement input. Offline
preparation produces artifacts for all three databases without starting services.
`tools/ingestion/test_semantics.py` checks typed values, the example's native
artifacts/parallel edges, missing endpoints, duplicate/cross-kind global IDs,
mandatory JSONL edge IDs, and LDBC equal IDs across types on the same owner. The
existing SNB/JSONL parity tests and BFS contract/concurrency tests remain the
compatibility checks; the new example is not the raw-source adapter in #38 or
the live onboarding workflow in #40.
