# Runtime graph metadata and source roots

The runtime uses logical schema names, normalized GlobalIds and configured ID
owners. Node names such as `Device` and `Site`, and edges such as `INSTALLED_AT`,
work without LDBC catalog changes. Physical labels, collections, SQL types,
endpoint queries and proxy handling remain in their existing backend adapters.

## Input path

| Stage | Implementation and responsibility |
| :-- | :-- |
| Domain configuration | `cli::load_domain_manifest` in `src/cli/DomainManifest.cpp` reads schema/data/backend paths and BFS defaults. Relative paths inside `domain.json` resolve against its directory. |
| Runtime construction | `cli::parse_runtime_config` and `GraphRuntimeBuilder::build` load the logical schema, validate metadata/ID layout, register stores and configure the existing placement catalog. |
| Metadata | `GraphDataReader` reads JSON or JSONL metadata. Schema type names become runtime-local TypeIds; those integers are not external source IDs. Cache/GPU plan metadata can supply the existing routing declarations. |
| Root selection | `cli::parse_source` accepts a canonical GlobalId or resolves a typed source identity through `SourceNodeIdResolver` and the prepared SQLite index. |
| Routing/remapping | `DatabaseContext::node_ref` retains existing optional native-ID-map remapping and ID-owner routing. Owner location bits are fixed backend bits, not positions in a dataset-specific type catalog. |
| Filters | `cli::parse_expand_spec` resolves `--allowed-edge-types` and `--allowed-target-node-types` against `LogicalGraphSchema`. Unknown logical names are errors. |
| Traversal | `BfsExecutor` and `PlacementCatalog` retain their existing direction, filtering, depth, replication, cache and visited-node behavior. |
| Physical queries | `Neo4JQueryVisitor`, `ArangoQueryVisitor` and `PostgreSqlGraphStore` obtain type names from schema metadata and own their backend query syntax. No query operators or database formats are added here. |

The CLI boundary is `app/bfs/main.cpp`. Benchmark lists and the HTTP public
request format continue to use GlobalIds; no new public query language or HTTP
payload syntax is introduced. Existing LDBC `--start-uuid` and native-ID-map
workflows remain supported.

## Select a typed source root

For a linked adapter using `typed-external-v1`, use the source identity that the
adapter emitted. This is the same tuple used by normalization and endpoint
resolution; it is not a scan of properties such as `external_id` or `ldbc_id`.

```bash
build/cpu-tests/bin/polygraph_bfs \
  --domain /path/to/graph/domain.json \
  --start-source '{"namespace":"inventory","type":"Device","idType":"string","id":"sensor:7"}' \
  --allowed-edge-types INSTALLED_AT \
  --allowed-target-node-types Site \
  --max-depth 2 --direction directed
```

`--start-source` requires exactly four JSON fields:

| Field | Meaning |
| :-- | :-- |
| `namespace` | Exact graph-scoped source namespace emitted by the adapter. |
| `type` | Exact logical vertex type, declared in the runtime schema. |
| `idType` | `string`, `int64` or `uint64`. |
| `id` | A JSON string for `string`, or an exact in-range JSON integer for either integer type. |

Signed integer `7`, unsigned integer `7` and string `"7"` remain distinct.
Explicit `idType` disambiguates positive signed values after JSON parsing.
Strings retain whitespace, case, Unicode and spelling; no implicit numeric
conversion, trimming or property fallback occurs. Floats, booleans, nulls,
composites, out-of-range values and unknown fields are rejected.

The runtime reads `paths.ingestionArtifacts/manifest.json` from the domain:

```json
{
  "format": "polygraph.domain",
  "version": 1,
  "id": "inventory",
  "paths": {
    "schema": "schema.json",
    "data": "runtime-data.jsonl",
    "backendConfig": "backends.json",
    "ingestionArtifacts": "native-artifacts"
  },
  "defaults": {
    "bfs": {
      "startSource": {
        "namespace": "inventory", "type": "Device", "idType": "string", "id": "sensor:7"
      },
      "maxDepth": 2,
      "direction": "directed"
    }
  }
}
```

Alternatively, `--source-index /path/to/native-artifacts/manifest.json` explicitly
selects the **prepared manifest**, not the SQLite filename. That CLI path follows
the process working directory; domain paths remain relative to the domain file.
The manifest must be prepared version 1/engine 1 with `typed-external-v1`. Lookup
opens its confined, size-checked `index.path` with SQLite read-only flags and
performs an indexed vertex-key query. The index stores node type and owner; type
and routing mismatches fail before BFS. No full source-key dictionary or graph is
loaded into memory.

Prepare/import with the existing [native engine](BULK_INGESTION.md#generic-graph-import)
first, and load the corresponding graph into the configured stores before
traversing. The stock importer still provides LDBC and canonical JSONL adapters;
this root selector does not create another source parser. See
[linked typed adapters](BULK_INGESTION.md#typed-external-ids-in-a-linked-adapter).
`paths.data` remains normalized runtime JSON/JSONL metadata/data, never raw adapter
CSV input. Keep schema, routing, backend contents and prepared artifacts from the
same graph generation together.

## Precedence and compatibility

- Explicit `--start-uuid` or `--start-source` overrides domain root defaults.
  Providing both CLI selectors is an error.
- Without an explicit selector, use `defaults.bfs.startSource` or
  `defaults.bfs.startUuid`. A domain declaring both is ambiguous and rejected.
- `--start-uuid` retains exact GlobalId parsing and optional native-ID-map
  remapping. Canonical roots do not open `ingestionArtifacts` or `--source-index`.
  Existing LDBC and JSONL domains need no new root fields or artifacts.
- Typed roots also pass through the existing native-ID-map resolver if configured.
  Runtime filters are resolved from logical schema names and do not use raw source
  IDs, LDBC ID blocks, or source properties to infer types.
- Runtime now links the existing SQLite dependency for indexed source lookup,
  independently of whether the bulk importer is built. Rebuild statically linked
  custom adapters after the shared `SourceIdentity` declaration moves into the
  logical model; the ingestion alias, helper API and v1 encoded key bytes remain
  source-compatible and unchanged.

## Verification limits

Prepared artifacts must remain immutable while a runtime uses them. Root lookup
checks version/state/strategy, path confinement, file size and the selected
mapping. It does not hash the entire index or confirm that live backend data is
fresh. Use `polygraph_bulk_import verify --manifest ... --strict` (also performed
by the generic import workflow) and existing live verification before running.
There is no automatic graph migration, index rewriting, service start or import
retry during root lookup.

[`tests/runtime_source_test.cpp`](../../tests/runtime_source_test.cpp) prepares
real typed-source artifacts, builds a runtime with generic schema names, resolves
multiple exact ID types, executes the real BFS executor against existing one-hop
test stores, and checks type/edge filters and reverse traversal. It also exercises
the actual BFS executable at depth zero without database requests, plus legacy
native-ID-map roots and read-only/malformed-index regressions. Live database
traversal and GPU execution require separate integration environments.

Python `polygraph.py run --graph-manifest ...` now accepts the same typed root
selector and domain default, forwarding resolution to C++. See
[Graph-aware Python tools](GRAPH_TOOLS.md) for measurement outputs and legacy
benchmark/reference compatibility.
