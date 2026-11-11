# Offline adapter contracts

The reusable suite in `tests/support/adapter_contract.py` exercises linked input
adapters through the existing native `prepare` and strict `verify` commands.
It needs no running database, container, network service or dataset converter.

```sh
cmake --preset cpu-tests
cmake --build --preset cpu-tests -j2
ctest --test-dir build/cpu-tests -R python.adapter_contracts --output-on-failure
```

For a direct run, provide the built executables:

```sh
POLYGRAPH_BULK_IMPORT="$PWD/build/cpu-tests/bin/polygraph_bulk_import" \
POLYGRAPH_MOVIE_IMPORT="$PWD/build/cpu-tests/bin/polygraph_movie_import" \
python3 -m unittest tests.test_adapter_contract -v
```

Normal CTest supplies those paths. If examples are disabled, built-in adapter
cases still run and the movie cases are explicitly skipped. A missing executable
environment variable is a skip, not a pass; an invalid supplied path is an error.

## Shared assertions

One mixin supplies the same nine tests to each declared case:

- Exact node/edge counts, logical types, owners and complete property sets.
- Exact JSON value types, including booleans versus integers, float versus integer,
  strings, arrays and nulls.
- Canonical ID preservation or exact typed source identity lookup, node/edge ID
  uniqueness, owner routing and endpoint closure.
- Edge orientation and independently declared directed/undirected reachability.
- Equal normalized records, source-ID maps, SQLite index metadata/checksum and
  native artifact checksums across fresh output directories and thread counts.
- Idempotent reruns that leave the published manifest unchanged.
- Duplicate node/relationship behavior, explicitly declared per source contract.
- Malformed input and missing-reference failures with no published success manifest.
- Unknown adapter rejection before any output directory is created.

Every preparation also checks that the copied input/config bytes and modification
times are unchanged. Additional corruption checks deliberately change logical
types, remove a property column, coerce property values, collide IDs and reverse
edge endpoints; the assertions must reject those snapshots.

This checks offline records and generated storage artifacts. It does not claim
live query equivalence, CUDA execution, performance bounds or concurrent mutation
support. Dataset-specific parser unit tests and the existing BFS executor/oracle
suite remain complementary coverage.

## Current cases

| Case | Reader | Oracle and purpose |
| --- | --- | --- |
| LdbcCsv | `ldbc-snb` | SNB snapshot, auxiliary arrays, Unicode, legacy IDs and relationship row semantics. |
| CanonicalSnbJsonl | `polygraph-jsonl` | The same logical oracle as LDBC, including exact canonical IDs and properties. |
| MixedJsonl | `polygraph-jsonl` | Device/Site/Gateway/Sensor types, unfamiliar columns, repeated property IDs, parallel edges, null/array/bool/float values and uint64 IDs above signed range. |
| MovieCsv | `movie-graph` | Typed source identities reused across node/relationship types, typed CSV properties and a three-hop oracle. |

Duplicate node identities are rejected in all current cases. Repeated canonical
or typed relationship IDs are also rejected. LDBC relationship rows have generated
IDs rather than a supplied relationship identity: an identical repeated source
row remains a distinct parallel edge. The harness checks this declared behavior
rather than forcing a deduplication policy onto every adapter.

## Adding another adapter case

1. Build an application containing the reader and expose its executable through
   an environment variable in the test command/CMake wiring.
2. Add a small source fixture and a coherent placement file. The harness copies
   them to a temporary directory before running or mutating anything.
3. Add one entry to `tests/fixtures/adapter_contract/cases.json` with `name`,
   `adapter`, `binary_env`, repository-relative `input`/`placement`, expected-file
   name, `id_strategy` and mutation recipes. No new test class or assertion file
   is needed: the driver generates cases from this declaration.
4. Write an independent expected-record file. Each node/edge has a unique `ref`,
   `identity`, `type`, `owner` and complete `properties`. Edges reference node
   aliases through `from`/`to`. Supply at least one non-self edge and properties
   so corruption checks can exercise meaningful failures.
5. Add directed and undirected traversal expectations: `root`, `direction` and
   `layers`, where each layer lists newly discovered aliases at that depth.
   Empty layers are explicit. These expected layers are supplied by the fixture
   author, not generated from normalized output.
6. Supply all four negative mutation recipes and run the suite. Do not generate
   expected logical records from the production adapter's prepared output.

Canonical identity example:

```json
{"uuid": 281474976710657}
```

Typed source identity example:

```json
{"source":{"namespace":"my-dataset","type":"Device","idType":"int64","id":7}}
```

The harness uses the documented v1 source-key protocol to locate that exact node
or relationship identity. `string`, `int64` and `uint64` remain distinct selectors.
Aliases are only fixture names; they never become graph IDs.

Mutation entries are `duplicate_node`, `duplicate_edge`, `missing_reference` and
`malformed`. Each names an input-relative `file` and one operation:

- `append_line`: append a zero-based physical line from the fixture (suitable for
  single-line records); or `append`: append explicit text.
- `replace`: replace exactly one declared old/new text pair.
- `json_line` plus `patch`: update fields of one zero-based JSONL record.

Rejected mutations declare an expected diagnostic substring in `error`.
`duplicate_edge` declares `policy: reject`, or `policy: preserve` with the alias
of the expected logical `edge` whose multiplicity increases. Recipes describe
source layout in fixture data; shared assertions contain no dataset-name,
logical-type catalog or property-column conditionals.

## Verification for #39

The shared matrix passed 36 tests in about two seconds. With the movie executable
unconfigured, the 27 built-in-reader tests passed and 9 movie tests were explicitly
skipped. Full CPU CTest passed 596 tests with 3 optional/live skips (599 registered);
the full Python suite passed 436 tests and 3,365 subtests. Build, documentation,
links, formatting and review checks passed. Live databases and CUDA were not used.
