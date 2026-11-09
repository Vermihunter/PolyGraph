# Movie graph: a linked CSV adapter

This miniature graph onboards **9 vertices and 9 relationships** through the
existing PolyGraph ingestion engine. External ID `1` is reused for Person, Film,
Studio, ACTED_IN, DIRECTED and PRODUCED_BY. These identities remain distinct:
namespace, entity kind, logical type and exact signed integer ID form the key.
There are no LDBC block IDs, source columns, filename conventions or backend
special cases in the reader.

## Build and import

From the repository root, using the existing CPU preset and its normal
[build prerequisites](../../docs/guides/BULK_INGESTION.md):

```sh
cmake --preset cpu-tests
cmake --build --preset cpu-tests --target polygraph_movie_import -j2
python3 examples/movie-graph/onboard.py --binary build/cpu-tests/bin/polygraph_movie_import
```

The last command is offline. It invokes the shared native `import --graph-manifest`
workflow, which prepares and strictly verifies native files for Neo4j, ArangoDB
and PostgreSQL. It neither connects to databases nor provisions services. Its
JSON result includes counts, the prepared manifest and the generated graph
manifest used for later runtime source-root lookup.

Run that same command again to verify/reuse the same generation. Edit CSV values
or append valid rows, then run it again: a new immutable generation is selected,
leaving the previous one intact. No C++ changes are needed for data edits.
`--manifest /path/to/domain.json` selects a copied example configuration and its
relative CSV/config paths. `--workspace-root /path/to/generated` overrides the
launcher's output base.

The launcher fingerprints CSV/config contents, the source manifest path and the
linked importer executable. By default it writes under
`build/example-workspaces/movie-graph-onboarding/`, with generated manifests in
`manifests/` and native output in `generations/<fingerprint>/ingestion/`. It only
selects a generation and calls the existing importer; it does not parse CSV,
allocate IDs, write storage records or implement another ingestion pipeline.
Keep the inputs unchanged during one import. Rebuilding the executable also
selects a new generation conservatively.

You can call the shared CLI directly when managing immutable output directories:

```sh
build/cpu-tests/bin/polygraph_movie_import adapters
build/cpu-tests/bin/polygraph_movie_import import \
  --graph-manifest examples/movie-graph/domain.json --dry-run
```

Direct import without `--dry-run` uses the manifest's fixed workspace. As with
other native imports, changed inputs need a fresh output directory. The launcher
above handles that selection for the repeated edit/import command. See
[graph workspaces](../../docs/guides/GRAPH_WORKSPACES.md) and the
[shared import workflow](../../docs/guides/GRAPH_MANIFESTS.md).

## CSV contract

Only these six files are consumed. Headers are required in the listed order.
IDs, endpoint IDs, years and fees are exact signed int64 decimals; ratings and
budgets are finite doubles; booleans are exactly `true` or `false`. Text is UTF-8.
Comma separators, doubled double quotes, quoted newlines and LF/CRLF are supported.
Empty numeric/boolean cells, malformed quoting, duplicate identities and unknown
endpoints fail instead of being coerced. This example has no optional/null cells.
Records are bounded by the engine's `max-record-mb` option.

| File | Columns | Logical type |
| --- | --- | --- |
| `csv/people.csv` | id, name, birth_year, active | Person |
| `csv/films.csv` | id, title, release_year, rating | Film |
| `csv/studios.csv` | id, name, independent | Studio |
| `csv/acted_in.csv` | id, person_id, film_id, role, credited | ACTED_IN: Person → Film |
| `csv/directed.csv` | id, person_id, film_id, fee | DIRECTED: Person → Film |
| `csv/produced_by.csv` | id, film_id, studio_id, budget | PRODUCED_BY: Film → Studio |

Person is owned by Neo4j, Film by ArangoDB and Studio by PostgreSQL.
Relationship ownership is independently configured in `placement.json`; native
writers generate the cross-backend adjacency replicas and proxies.

## Roots and independent BFS oracle

`expected.json` lists exact newly reached layers, not cumulative counts. A label
such as `Film:1` denotes a typed external ID, not a canonical GlobalId.

| Root | Direction | 1 hop | 2 hops | 3 hops |
| --- | --- | --- | --- | --- |
| Person:1 | undirected | Film:1 | Person:2, Person:4, Studio:1 | Film:2 |
| Film:2 | undirected | Person:2, Person:3, Studio:1 | Film:1, Film:3 | Person:1, Person:4, Studio:2 |
| Person:1 | directed | Film:1 | Studio:1 | none |

For example, the bridge Person:1 → Film:1 — Person:2 → Film:2 needs three hops
when directions are ignored. The Film:2 root reaches all nine vertices by depth
three, crossing every node type and backend owner.

Use explicit typed selectors when resolving roots:

```json
{"namespace":"movie-graph","type":"Person","idType":"int64","id":1}
```

```json
{"namespace":"movie-graph","type":"Film","idType":"int64","id":2}
```

For the checked-in fixture/version, the prepared source index resolves these to
GlobalIds `281474976710656` and `562949953421317`, respectively. They are **not**
computed from the external ID alone and may change when inputs or placement
change. The generated graph manifest contains a Person:1 default source selector
and the correct ingestion index path.

`runtime-metadata.jsonl` supplies routing metadata, not an in-memory graph copy.
Backend connection settings are illustrative and contain no credentials. Before
running distributed BFS, import into configured fresh targets through the
[existing execution workflow](../../docs/guides/DOCKER_INGESTION.md). This issue's
tests are offline and do not claim live database or CUDA execution.

## Extension boundary

`MovieAdapter.cpp` contains all dataset-specific CSV headers, types and property
mappings. `main.cpp` copies the built-in registry, adds this adapter and calls
`run_bulk_import_cli`. Its registry is recreated on every executable invocation,
including any existing Docker helper re-entry. The stock `polygraph_bulk_import`
still lists only `ldbc-snb` and `polygraph-jsonl`.

One additive contract gap was fixed for this proof: the old executable hardwired
the default registry even though the engine accepted a custom registry. The
existing CLI implementation is now linked as `polygraph::ingestion_cli` and accepts
an application-owned registry. No normalization, ID allocator, generic Python
script or backend writer gained movie-specific behavior. Future linked adapters
can use this composition hook without changing the shared CLI. Bump the adapter
version when changing mapping or identity semantics; data-only edits need no code change.

Tests cover CSV types/quoting/errors and synchronous cancellation; shared CLI
registry discovery; normalization and strict native artifacts; independent
1–3-hop expectations; unchanged-input reuse; and CSV edits preserving old
generations. Run them after building the test targets:

```sh
cmake --build --preset cpu-tests -j2
ctest --test-dir build/cpu-tests -R 'Movie|movie_graph|BulkImportCli' --output-on-failure
```

Disable example applications with `-DPOLYGRAPH_BUILD_EXAMPLES=OFF` when configuring
a build that does not need them.

Verification for #38: the full CPU suite passed 595 tests, with three optional/live
cases skipped; the full Python suite passed 400 tests and 3,331 subtests. The
example's focused CSV/CLI/import/oracle tests and the documented onboarding command
passed. Documentation/link/format checks passed; independent review reported no
actionable findings. Live database and CUDA execution were not run.
