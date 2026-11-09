# Graph manifests and read-only preflight

Native ingestion accepts the existing `polygraph.domain` **version 1** format
with an additive `ingestion` section. There is no second canonical graph format.
The manifest selects one graph, a statically linked adapter, identity strategy,
source/configuration paths and native artifact directory. Existing BFS/runtime
domain loaders continue reading their existing fields and ignore this extension.

Use `polygraph_bulk_import import --graph-manifest domain.json` (or
`python3 -B tools/polygraph.py import --graph-manifest domain.json`) as the generic
native ingestion entry point. It prepares and strictly verifies artifacts offline.
Add `--dry-run` for read-only configuration/inventory validation, or
`--execute --confirm-create` for existing native execution on configured isolated
fresh targets. See [Generic graph import](BULK_INGESTION.md#generic-graph-import)
for reports, safety boundaries and equivalent LDBC/JSONL examples. Legacy
`validate` and `prepare` commands below remain available.

## Validate and prepare

Build the importer separately, then run from the repository root:

```bash
cmake --build --preset cpu-tests --target polygraph_bulk_import
build/cpu-tests/bin/polygraph_bulk_import validate \
  --graph-manifest docs/examples/ingestion/logical-graph/domain.json
build/cpu-tests/bin/polygraph_bulk_import prepare \
  --graph-manifest docs/examples/ingestion/logical-graph/domain.json
```

The checked-in [canonical JSONL example](../examples/ingestion/logical-graph/domain.json)
uses Device/Site/Gateway/Sensor types and all three persistent backends. The
[LDBC fixture manifest](../examples/ingestion/ldbc.domain.json) selects `ldbc-snb`
and raw CSV input while retaining a separate canonical runtime data path.
Both validate offline; preparation reuses existing normalization and backend writers.
Examples write generated artifacts under ignored `build/graph-manifests/` only
when **prepare** is requested.

The Python entry point delegates to the same native validation implementation:

```bash
python3 -B tools/polygraph.py validate \
  --graph-manifest docs/examples/ingestion/logical-graph/domain.json \
  --binary build/cpu-tests/bin/polygraph_bulk_import
```

It does not build the executable or provision services. Its default executable
is `build/cpu-tests/bin/polygraph_bulk_import`; use `--binary` for another build.
Direct script invocation disables bytecode generation before local imports;
`-B` also prevents interpreter/module-loading caches when embedding the entry point.

`prepare --graph-manifest` cannot be combined with legacy preparation flags;
put options in the manifest or use the old flags alone. `load` and `execute`
retain their existing interfaces. Generic `import --graph-manifest` also supports
explicit native execution with `--execute --confirm-create`.

## Manifest fields

This abbreviated example uses the same runtime schema/backend config files as
an existing domain:

```json
{
  "format": "polygraph.domain",
  "version": 1,
  "id": "inventory",
  "paths": {
    "schema": "schema.json",
    "data": "graph.jsonl",
    "backendConfig": "backends.json",
    "placementConfig": "placement.json",
    "ingestionArtifacts": "artifacts/native",
    "targetConfig": "targets.json"
  },
  "ingestion": {
    "adapter": "polygraph-jsonl",
    "identity": {"strategy": "preserve-canonical-v2"},
    "options": {"threads": 3, "memory-limit-mb": 256}
  },
  "defaults": {"bfs": {"startUuid": "281474976710657"}}
}
```

| Field | Meaning |
| :-- | :-- |
| `format`, `version` | Exactly `polygraph.domain` and integer `1`; unsupported versions fail. |
| `id` | Required nonempty graph identifier for native ingestion. `name`, runtime/defaults and other domain fields retain their runtime meanings. |
| `paths.schema` | Existing logical schema, with `nodeTypes`, `relations`, named properties and endpoint types. Required and readable during preflight. |
| `paths.data` | Existing runtime graph/descriptor path. Native input defaults to it; a separate `sourceData` does not change its meaning. |
| `paths.sourceData` | Optional raw/native input file or directory. Set it for raw LDBC snapshots or when runtime data is a separate artifact. |
| `paths.backendConfig` | Existing runtime backend config. Preflight checks a `backends` array and an enabled backend kind for each persistent placement backend; it does not construct runtime stores or validate every runtime capability. |
| `paths.placementConfig` | Existing native placement JSON: supported backends, type-owner maps and defaults. Required. |
| `paths.ingestionArtifacts` | Native preparation output directory. Optional; defaults to `<workspace>/ingestion`. Validation creates no directories. |
| `paths.targetConfig` | Optional existing native target JSON. When supplied, it must be readable and pass existing named-target/credential-storage rules even for offline validation. Required for `--execution`. |
| `paths.partitionFile`, `paths.partitionMap` | Optional existing external assignment/map files, with the same dependencies as native CLI flags. |
| `ingestion.adapter` | Required stable registry name. `polygraph_bulk_import adapters` lists available names. |
| `ingestion.identity.strategy` | Optional explicit strategy, which must match the adapter. Defaults to `preserve-canonical-v2` for canonical adapters or `ldbc-block31-v3` for the LDBC policy. It cannot request a different allocator or rewrite IDs. |
| `ingestion.options` | Optional native preparation options below. Unrecognized fields are errors, not ignored overrides. |

The native reader resolves the manifest filename relative to the invocation
working directory. **Every path inside the manifest resolves relative to its
containing directory**, including optional source, target and partition paths.
Absolute paths remain absolute; existing symlinks are resolved. No shell, tilde,
environment-variable or glob expansion is performed by the native reader.
Referenced target files retain the existing executor semantics: executable names
use `PATH`, and relative command paths use the invocation working directory.
Prefer absolute command/artifact-root paths for portable target configurations.

Native validation requires the source input, schema, placement and backend config
to exist. Runtime `data` may refer to an artifact not yet produced **when a separate
`sourceData` selects the actual input**. Optional target/partition files must exist
when declared. The output directory must be outside the input dataset and must
not contain any declared source/configuration files; this prevents native manifest
publication from overwriting the graph declaration or input files.

Options use existing CLI names without `--`:

| Option | Default |
| :-- | :-- |
| `partitioner` | `none` (`kaminpar` needs a capable build) |
| `seed` | `42` |
| `threads` | `3` |
| `memory-limit-mb` | `256` |
| `chunk-size-mb` | `64` |
| `max-record-mb` | `8` |
| `partitions` | `0` |
| `partition-threads` | `1` |
| `partition-imbalance` | `0.03` |
| `partition-preset` | `default` |

Numbers must have their declared JSON types; integer options cannot be booleans,
strings, negative values or values outside uint32. The engine's existing bounds
and partition-control dependencies still apply. Source namespaces, type remapping,
duplicate/endpoint skip policies and alternate ID encodings are not new options;
see the [logical graph contract](../architecture/logical-graph-contract.md).

## What validation checks

`validate` reads configuration, canonicalizes/enumerates inventory paths and checks
read access. It does **not** hash the entire dataset or normalize its records.
Preparation still hashes input/configuration contents and checks them again before
publication. A declaration/schema/backend/target configuration change invalidates
reuse of a manifest-based generation; legacy native CLI fingerprints remain unchanged.

Schema checks include unique type/property names, known scalar property types,
declared edge endpoint types, boolean direction and sanitized/reserved names.
Placement maps must name declared types and enabled persistent backends. When
Arango is selected, vertex/relationship names must not share a collection name.
Raw-source declared types need explicit/default routes; canonical owners remain
encoded in IDs and cannot be silently reassigned.

Built-in inspection is read-only and bounded: JSONL checks leading routing metadata
and the native 16/48-bit layout; LDBC checks snapshot table names/types without
building its auxiliary SQLite index. Registry definitions can supply an optional
read-only `inspect` callback. For an adapter without it, the report explicitly says
`source_inspection: inventory-only`; full parsing remains preparation's job.

Output checks inspect the nearest existing ancestor's permissions/access. They
create no directories, locks, temporary files, write probes or report files.
Success prints JSON to stdout, including the selected graph/adapter, strategy,
inventory count, output path, and `not_checked` list. Failures also leave inputs
and output paths unchanged. Normal filesystem read-access timestamps remain an
operating-system concern; validation does not modify contents or modification times.

### Execution prerequisites

```bash
build/cpu-tests/bin/polygraph_bulk_import validate \
  --graph-manifest domain.json --execution
```

This additionally checks compiled native executor support, isolated target names,
HTTP endpoint form, PostgreSQL port, declared credential environment variables,
Arango import limits and Neo4j offline/argv configuration. Runner/preflight/start
executables are checked for availability/access, **never invoked**. Referenced
environment variable values and target credentials are not printed.

There is no database connection, DNS request, Docker invocation, service start/stop,
operator command or importer call. Consequently success does not prove target
freshness/connectivity, operator-check results, import permissions, storage capacity
or future filesystem availability. Full record uniqueness, property representability
and endpoint closure are checked by preparation, not preflight. Native execution
still performs its existing fresh-target checks and requires explicit authorization.

## Diagnostics and compatibility

Native validation errors use stable category prefixes: `graph.manifest:`,
`graph.format:`, `graph.version:`, `graph.id:`, `graph.paths:`, `graph.ingestion:`,
`graph.adapter:`, `graph.options:`, `graph.input:`, `graph.schema:`,
`graph.placement:`, `graph.backends:`, `graph.identity:`, `graph.output:` and
`graph.execution:`. The following text identifies the offending field/path/type;
do not depend on byte-for-byte prose. Native argument/JSON errors use exit 2,
preflight failures use exit 3, and unexpected I/O/internal errors use exit 5.
The Python wrapper reports native diagnostics and returns its existing failure
status 1 (`argparse` usage errors remain 2).

Existing runtime `domain.json` files remain valid for C++/Python runtime loaders.
There is no reliable inference of a raw adapter, placement or artifact directory
from an old domain's name or scale factor. To opt into native validation/preparation,
keep its runtime fields and add `ingestion.adapter` and `paths.placementConfig`.
Optionally override `paths.ingestionArtifacts`, or use its workspace default; add raw `sourceData`, target and identity
fields. The validator gives a `graph.ingestion:` migration diagnostic when this
native extension is absent. No migration command rewrites existing manifests.

Existing `prepare --adapter ... --input ... --placement-config ... --output-dir ...`
and Docker/execution workflows are unchanged. Native prepared `manifest.json`
(checksummed output) and user `domain.json` (input configuration) remain distinct;
pass the former to `verify`/`execute`, and the latter to `validate --graph-manifest`
or `prepare --graph-manifest`.

Linked adapters using `InputIdPolicy::typed_external` select the versioned
`typed-external-v1` identity strategy. Source namespaces and exact typed values
are supplied by the adapter; preflight validates configuration/inventory, while
preparation resolves and checks record identities in SQLite. See
[typed external IDs](BULK_INGESTION.md#typed-external-ids-in-a-linked-adapter).

Runtime BFS can select `defaults.bfs.startSource` or `--start-source` through the
prepared typed-source index under `paths.ingestionArtifacts`. Canonical
`startUuid` remains supported. See [Runtime graph metadata](RUNTIME_GRAPH_METADATA.md)
for exact ID types, selector precedence and verification boundaries.

Generated output defaults and collision checks are described in [graph workspaces](GRAPH_WORKSPACES.md).
