# Graph-aware Python tools

For a new graph, provide a `polygraph.domain` version 1 manifest and use the
existing native import workflow. Generic commands select a graph through
`tools.graphio.domain.resolve_graph_manifest` / `resolve_domain`; no LDBC script
needs copying or editing. Python selects configuration and launches tools. The
native engine owns parsing/normalization, IDs, placement, proxies, native writers,
execution safeguards and typed source-root resolution.

## Select a graph

The Python entry points accept:

- An explicit manifest file, such as `/datasets/inventory/domain.json`.
- A graph directory containing `domain.json`.
- A domain ID or directory name discovered under `--data-root`. Without that
  option, legacy discovery still uses repository `data/`.

File/directory arguments resolve from the invocation working directory. Relative
paths **inside** a domain remain relative to its manifest directory, including
schema, data, backends, cache plans and ingestion artifacts. ID discovery rejects
ambiguous matches and ignores unrelated/malformed JSON. Explicit files go to the
selected command for validation, so native import diagnostics stay authoritative.
Use literal paths in native manifests; native path rules do not expand environment
variables. Legacy Python domain readers retain their existing path expansion.

```bash
# One native import entry point; default is offline preparation/strict verification.
python3 -B tools/polygraph.py import --graph-manifest /datasets/inventory
python3 -B tools/polygraph.py import --graph-manifest inventory --data-root /datasets

# Read-only checks; these do not build binaries, invoke Docker or write artifacts.
python3 -B tools/polygraph.py validate --graph-manifest /datasets/inventory
python3 -B tools/polygraph.py import --graph-manifest inventory --data-root /datasets --dry-run

# Execution remains an explicit choice for configured isolated fresh targets.
python3 -B tools/polygraph.py import --graph-manifest /datasets/inventory --execute --confirm-create

# Discover external graph configurations.
python3 -B tools/polygraph.py list --data-root /datasets
```

`--binary` selects the native importer when it is not at
`build/cpu-tests/bin/polygraph_bulk_import`. Adapter selection comes from the
manifest and the executable's linked registry. Python does not duplicate that
registry. See [native import](BULK_INGESTION.md#generic-graph-import) for target
configuration, artifact reports and read-only validation limits.

## Runtime, caches and measurements

`run`, cache commands, GPU-storage planning and `benchmark-caches` accept the
existing positional domain or `--graph-manifest`, plus optional `--data-root`.
Use exactly one selector. Place legacy positional domains before options with
multiple values (such as `--config`), or use the explicit manifest flag to avoid
argparse's greedy multi-value parsing.

```bash
# Existing positional syntax still works.
python3 tools/polygraph.py run inventory --data-root /datasets --start-uuid 281474976710657

# Typed IDs are forwarded unchanged to C++; no Python SQLite/ID allocator is added.
python3 tools/polygraph.py run --graph-manifest /datasets/inventory \
  --start-source '{"namespace":"inventory","type":"Device","idType":"string","id":"sensor/7"}' \
  --binary build/cpu-tests/bin/polygraph_bfs \
  --output-dir /results/inventory-run --run-id inventory-run --repetitions 3

# Domain defaults may provide startUuid or startSource; explicit roots override them.
python3 tools/polygraph.py run --graph-manifest /datasets/inventory \
  --output-dir /results/another-run -- --max-depth 2 --direction directed

python3 tools/polygraph.py cache build --graph-manifest /datasets/inventory \
  --config /configs/cache-small.json --output-dir /results/inventory-cache
```

Import-only manifests need no BFS default. A run without an explicit or configured
root fails before creating measurement outputs. Root options before and after
`--` cannot conflict. `--source-index` can be forwarded after `--` when needed;
otherwise C++ uses `paths.ingestionArtifacts`. See
[runtime source roots](RUNTIME_GRAPH_METADATA.md).

The existing measurement default stays `measurements/<run-id>`. `--output-dir`
selects the measurement directory directly. Numeric root directory names and CSV
columns retain their previous semantics. Typed-root measurements add a
`start_source` JSON column, leave `start_uuid` empty and use a hash of that JSON
only as a filesystem label. That hash is never a graph ID. Existing overwrite
checks remain enabled; this does not redesign workspace or lifecycle ownership.

## Compatibility commands and migration

| Invocation | Behavior |
| :-- | :-- |
| `polygraph.py import --graph-manifest ...` | Preferred native workflow. |
| `polygraph.py load --graph-manifest ...` | Alias to that same native workflow; prepares offline unless execution is explicit. |
| `load_graph.py --graph-manifest ...` | Same native delegate, accepting dry-run/execute flags and `--binary`. |
| `polygraph.py load DOMAIN [--reset ...]` | Existing positional Python loader behavior, retained for compatibility. |
| `load_graph.py graph.jsonl [legacy endpoint flags]` | Existing direct Python loader, retained with its old endpoint/database defaults. |
| `prepare ldbc`, `generate ldbc`, `stats ldbc`, `quickstart ldbc` | Existing named adapter/support workflows and arguments. |

Manifest load aliases forbid legacy reset, connection or backend-selection flags.
They return migration messages pointing to native import or the unchanged legacy
invocation. Native import never resets existing data. No old positional command
is silently redirected into a different destructive lifecycle.

The generic reference BFS also accepts `--graph-manifest` (alias `--domain`) and
`--data-root`:

```bash
python3 tools/algorithms/bfs.py --graph-manifest /datasets/inventory \
  --output-csv /results/reference.csv
```

It reads normalized JSON/JSONL data and inherits numeric root, depth and direction
from the manifest. `--start-uuid`, `--max-depth`, `--directed` and `--undirected`
explicitly override those defaults. Existing `--input`, `--input-json` and raw
`--ldbc-input-dir --schema` invocations remain available. The raw reconstruction
implementation now lives in `tools/ldbc_snb/reference_bfs.py`, with compatibility
wrappers in the generic script. This independent oracle still needs full graph
records; metadata-only runtime data is insufficient. Typed-source defaults require
an explicit canonical root for this oracle, or C++ `polygraph.py run`; Python does
not recreate the core source resolver.

## Portable benchmark configuration

Existing benchmark suite paths remain repository-relative. For a configuration
outside the repository, opt in to `"pathBase": "config"`:

```json
{
  "name": "inventory-suite",
  "pathBase": "config",
  "datasets": [{"name": "inventory", "domain": "graphs/inventory/domain.json"}],
  "graphDbBinary": "bin/polygraph_bfs",
  "outputDir": "results",
  "dockerCompose": "compose.yml"
}
```

This mode resolves dataset manifest paths, root-list files, explicit binary,
Compose file and output paths relative to the suite file. The default repository
Compose file remains repository-owned when omitted. Absolute paths are unchanged.
`prepare.command` is still explicit argv executed with the existing repository
working directory; use absolute paths in those commands. It can call the generic
native import entry point rather than a dataset-specific script.

Existing `composeInstance` scale/distribution aliases and `composeProject` remain
usable. Typed-root graphs need canonical `startUuids`/`startUuidsFile` for the suite
framework, or can use `polygraph.py run --start-source --repetitions ...` directly;
missing numeric roots get an explicit replacement message before service startup.
Cache benchmarks likewise check numeric roots before replacing existing results.
Suite config inspection does not import optional plotting dependencies; actual
suite execution still checks those dependencies before lifecycle work.

## Survey and extension boundaries

| Area inspected | Result |
| :-- | :-- |
| `polygraph.py`, `graphio/domain.py` | Shared path/directory/ID selection; generic graph aliases and optional root metadata. New imports delegate to the native engine. |
| `tools/load_graph.py` | Native manifest delegate added; old direct loader remains an explicit compatibility route. |
| `tools/algorithms/bfs.py` | Generic graph/config loading and independent adjacency BFS retained; LDBC parsing/catalog imports moved out. |
| `tools/ldbc_snb/` | Owns raw CSV reconstruction, scale/root-directory discovery, default LDBC paths, Datagen parameter editing, conversion and statistics. Polygraph helper aliases are retained; imports are lazy. |
| `graphio` readers/planners/loaders | Existing generic implementations retained. Explicit `polygraph.ldbc-csv` format dispatch remains a lazy adapter boundary. |
| Root CSV output | Generic `external_id` property by default; adapter callers can pass `external_id_property`. LDBC preparation explicitly selects `ldbc_id`, preserving its CSV output. Direct custom callers relying on that old implicit property should pass it explicitly. |
| `measurement/benchmark_suite.py` | Existing orchestration retained, with opt-in external-config path resolution and numeric-root migration diagnostics. |
| SF matrices, native BFS/PageRank probes and checked-in experiment configs | Named dataset/research scenarios remain compatibility/support tools. They are not the onboarding path; no reset/stop/container ownership semantics changed. |
| University prepare/quickstart and legacy loader defaults | Named sample/compatibility workflows retained. New graphs use their own manifest rather than copying the sample or LDBC scripts. |

Tests in [`tools/test_graph_tools.py`](../../tools/test_graph_tools.py) exercise
external directories with spaces, ID discovery/ambiguity, root forwarding and
measurements, script-level reference defaults/overrides, native alias safety,
legacy CSV behavior, and fresh generic checkouts without the LDBC support package.
