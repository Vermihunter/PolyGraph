# Add a dataset without changing the ingestion engine

First follow the [quickstart](quickstart.md). Choose the smallest extension that
matches your source:

| Situation | Change |
| --- | --- |
| New rows in a supported source layout | Data/configuration only; keep the adapter and identity policy. |
| Canonical PolyGraph JSONL with new graph types | Select `polygraph-jsonl`; supply schema, placement and valid canonical IDs/endpoints. |
| An LDBC SNB CsvBasic snapshot | Select `ldbc-snb`; keep its compatibility policy and source layout. |
| Another CSV dialect, source keys, joins or format | Implement a linked input adapter, using the movie example as a working reference. |
| New database engine or physical storage convention | Backend writer/executor/runtime adapter work, separate from the dataset reader. |
| Query existing arbitrary relational tables without importing | Not supported by this onboarding path; a separate future capability. |

A manifest is configuration, not a generic column-mapping language. Changing a
CSV header in JSON does not teach an existing reader a new dialect. Conversely,
a new domain does not require reader code if canonical JSONL already represents it.
Do not introduce a second loader, ID allocator or deployment framework.

## Architecture and files

```mermaid
flowchart LR
    C[Graph manifest: schema, input, placement] --> A[Linked input adapter]
    A --> N[Existing normalization and identity resolution]
    N --> I[(SQLite index)]
    I --> W[Backend writers: nodes, proxies, replicas]
    W --> V[Strict artifact verification]
    V --> E[Explicit native execution]
    E --> B[Runtime backend adapters and BFS]
```

| Path | Responsibility |
| --- | --- |
| `include/ingestion/Import.hpp` | `Record`, `IGraphInputAdapter`, `RecordConsumer`, preparation and execution contracts. |
| `include/ingestion/InputAdapterRegistry.hpp` | `InputAdapterDefinition`, registry and ID policy selection. |
| `include/domain/graph/SourceIdentity.hpp` | Exact source identity tuple and runtime selector encoding. |
| `include/ingestion/BulkImportCli.hpp`, `app/bulk_import/BulkImportCli.cpp` | Shared CLI with an application-owned registry. |
| `examples/movie-graph/MovieAdapter.cpp` | Complete bounded CSV reader, property parsing and registration definition. |
| `examples/movie-graph/main.cpp`, `CMakeLists.txt` | Composition root and linked example targets. |
| `examples/movie-graph/domain.json`, `schema.json`, `placement.json`, `csv/` | Source declaration, logical types, ownership and fixture data. |
| `src/ingestion/` | Shared ingestion engine, existing native writers/executors and compatibility readers. |
| `tests/support/adapter_contract.py`, `tests/fixtures/adapter_contract/` | Reusable offline contract assertions and independently declared cases. |

Dataset-specific code owns source syntax, table/field mappings, auxiliary joins,
source identity namespace and conversion policy. Core owns bounded normalization,
identity allocation/lookup, partitioning, endpoint resolution, proxies/replicas,
artifact checksums and workflow orchestration. Backend adapters own SQL/Cypher/AQL,
physical labels/collections/tables, signed storage representations and import APIs.
Existing JSONL ingestion, BFS and caches already provide reusable behavior; a new
source adapter should not add dataset/type branches there.

## Implement and register a reader

Use [the complete movie implementation](../../examples/movie-graph/MovieAdapter.cpp)
and its [header](../../examples/movie-graph/MovieAdapter.hpp) as the copyable reference.
Its reader implements exactly these methods:

```cpp
void nodes(const polygraph::ingestion::RecordConsumer &consume) override;
void relationships(const polygraph::ingestion::RecordConsumer &consume) override;
polygraph::ingestion::Json metadata() const override;
```

`IGraphInputAdapter` is declared in `Import.hpp`. Stream nodes and then
relationships synchronously through `consume(Record)`; do not retain the graph,
queue callbacks after returning, or swallow consumer errors/cancellation. Bound
record memory using `PrepareOptions::max_record_mb` and identify malformed input
with source path and record context. The factory receives `PrepareOptions` plus
a scratch path; read-only inventory/inspection must not create scratch files.

For a typed external source, emit a node like this inside the reader:

```cpp
using namespace polygraph::ingestion;
Record record{};
record.type = "Film";
record.identity = SourceIdentity{"movie-graph", "Film", Json(std::int64_t{2})};
record.properties = {{"title", "Harbor Lights"}, {"release_year", 2010}};
consume(std::move(record));
```

For relationships set the logical edge `type`, its own `identity`, and both
`from_identity` and `to_identity` referencing vertex tuples. The movie reader's
`relationship()` helper shows this exactly. Leave legacy `key/from/to` strings
empty under `typed_external`. Let placement select ownership and the engine
allocate canonical IDs; do not encode backend bits or build proxy records here.

Return an `InputAdapterDefinition` containing:

- A stable case-sensitive `name` and nonempty `version`.
- `factory(options, scratch)` returning the reader.
- `input_files(options)` listing every source dependency deterministically,
  including auxiliary files; the engine fingerprints these contents.
- The explicit `id_policy`: normally `typed_external` for arbitrary source IDs.
- Optionally `inspect(options)` for bounded read-only source metadata checks.
  Without it, preflight reports inventory-only inspection; full parsing still
  happens during preparation.

Bump the adapter version when parsing, identity or mapping semantics change.
Ordinary data edits do not require a code/version change. Factories are linked
C++ code, not dynamic plug-ins. The application recreates its registry on every
invocation, including Docker helper re-entry. The movie composition root is:

```cpp
#include "MovieAdapter.hpp"
#include "ingestion/BulkImportCli.hpp"

int main(int argc, char **argv) {
    auto registry = polygraph::ingestion::default_input_adapters();
    registry.add(polygraph::examples::movie_graph::input_adapter());
    return polygraph::ingestion::run_bulk_import_cli(argc, argv, registry);
}
```

Copy the example's CMake structure: link the reader library to
`polygraph::ingestion`, and the executable to that library plus
`polygraph::ingestion_cli`. Do not fork `BulkImportCli.cpp` or modify the built-in
registry for each application. Verify the resulting executable's `adapters`
output, then select its registered name through `ingestion.adapter` in a copied
graph declaration. Use that executable for validation and every later import.

## Identity, edges and values

The [logical graph contract](../architecture/logical-graph-contract.md) is the
full reference. Preserve these rules:

- Identity is namespace + vertex/edge kind + logical type + exact value type/value.
  `int64(7)`, `uint64(7)` and string `"7"` differ. Use an explicit C++ integer type;
  JSON parsers can classify positive integers as unsigned. Do not round through
  floating point, trim strings or normalize case silently.
- Endpoint tuples identify vertices, including namespace and type. A relationship
  has its own identity and directed `from` → `to` endpoints. Query direction is a
  runtime choice; the reader does not duplicate reverse edges for undirected BFS.
- Duplicate vertices and repeated explicit edge identities fail. Distinct edge
  identities with equal endpoints are valid parallel edges. LDBC's identity-less
  relationship rows retain their existing occurrence semantics.
- Missing endpoints fail before a prepared manifest is published. Proxies represent
  known remote vertices; they do not repair dangling source references.
- Canonical JSONL `uuid` values already are global IDs. An `external_id` property
  does not become an identity key. Canonical owner bits are preserved; changing
  placement cannot silently move those records.
- Generated canonical IDs are tied to the complete preparation/placement context.
  Use typed root selectors and their matching index rather than assuming external
  ID N becomes GlobalId N. Treat ownership/identity changes as a new generation
  and explicit migration, not an in-place update.
- Preserve JSON value types and absent/null/empty distinctions. Portable values
  include signed integers, finite floats, booleans, text and homogeneous scalar
  arrays. Neo4j cannot store every nested JSON value and stores null as absence;
  unsupported representations fail rather than stringify silently.
- Schema declaration/preflight does not enforce every property at record level.
  The reader must parse its promised types. Avoid invented schema tags such as
  `nullable` or array types. Physical-name collisions and reserved metadata names
  are rejected; consult the contract before selecting names.

A source adapter does not need to know the persisted signed representation of a
uint64 GlobalId, SQL table names, Neo4j local payload conventions, or Arango handles.
Those existing backend responsibilities stay unchanged.

## Test before adding live services

Build all ordinary test targets, then run the existing shared suite:

```sh
cmake --build --preset cpu-tests -j2
ctest --test-dir build/cpu-tests -R python.adapter_contracts --output-on-failure
ctest --test-dir build/cpu-tests -R 'Movie|movie_graph|BulkImportCli' --output-on-failure
```

Follow [the contract-case recipe](../guides/ADAPTER_CONTRACT_TESTS.md) to add your
reader to `tests/fixtures/adapter_contract/cases.json`. Supply an executable
environment variable, copied source/placement fixture, independent expected
records/layers, exact identity policy and negative mutation recipes. The existing
driver generates the same tests for each case; do not add dataset-name branches
to shared assertions. Wire the executable variable into its CTest command as the
movie case already does.

Check exact properties/types, IDs/owners, endpoint closure, directed/undirected
layers, deterministic fresh output/thread counts, unchanged-input reuse,
duplicate behavior, malformed inputs, missing references and cancellation. Unit
tests for a new parser should exercise its quoting/encoding/bounds/conversions.
Do not generate the expected oracle from the adapter output being tested.

Only then use a real isolated integration run. The
[movie live workflow](../guides/MOVIE_LIVE_SMOKE.md) is an implemented fixture test,
not a promise that passing `--adapter <any-name>` automatically verifies another
source. Add suitable independent fixture assertions for another dataset while
reusing `DockerEnvironment`, generic import, runtime bootstrap and the existing
ownership-checked cleanup. Service-free tests and strict artifact verification do
not prove successful backend imports or live BFS.

## Compatibility checklist

Keep both built-in registry names and policies, old `prepare`/`execute`/Docker
interfaces, canonical JSONL parsing and BFS root/direction behavior intact.
Never add a dataset's logical types to a generic backend query just to make a test
pass. Validate copied declarations with the intended linked executable and use
fresh output/targets for semantic changes. See the quickstart's
[compatibility paths](quickstart.md#use-jsonl-or-ldbc-without-writing-an-adapter)
for LDBC and JSONL commands.
