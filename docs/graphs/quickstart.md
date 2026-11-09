# Onboard a graph and run BFS

Start with the checked-in movie graph: nine vertices and nine relationships,
with Person in Neo4j, Film in ArangoDB and Studio in PostgreSQL. Configuration
selects the reader, schema, placement and inputs. You only need adapter code when
your source format or identity mapping is new; see [adding a dataset](adding-a-dataset.md).

This workflow imports graph records into PolyGraph's supported storage layout.
It does not discover or query an arbitrary pre-existing relational database
schema. Connecting existing SQL tables without importing is a separate future
capability.

## Clone and build

Run commands from the repository root on Linux. Required tools are Git, CMake
3.27+, Ninja, a compiler supporting the project's C++23 build, and Python 3.10+.
Install development headers/libraries for PostgreSQL/libpq, SQLite3, OpenSSL and
libcurl. On Debian/Ubuntu these packages are `libpq-dev`, `libsqlite3-dev`,
`libssl-dev` and `libcurl4-openssl-dev`; install the tools through your distribution
or toolchain manager. The first CMake configure needs network access for missing
FetchContent dependencies (including spdlog and argparse), or locally supplied
sources/packages. See [build configuration](../guides/BUILDING.md).

```sh
git clone https://github.com/Vermihunter/PolyGraph.git
cd PolyGraph
cmake --preset cpu-tests
cmake --build --preset cpu-tests --target polygraph_bulk_import polygraph_movie_import polygraph_bfs -j2
```

No database service, CUDA, Redis or Python third-party package is required for
the offline steps below. Leave example applications enabled (the default).
`polygraph_bulk_import` contains the two built-in readers; `polygraph_movie_import`
adds the linked movie reader to the same CLI.

## Select, validate and prepare

```sh
build/cpu-tests/bin/polygraph_movie_import adapters
build/cpu-tests/bin/polygraph_movie_import validate \
  --graph-manifest examples/movie-graph/domain.json
build/cpu-tests/bin/polygraph_movie_import import \
  --graph-manifest examples/movie-graph/domain.json --dry-run
python3 examples/movie-graph/onboard.py \
  --binary build/cpu-tests/bin/polygraph_movie_import > build/movie-onboarding.json
```

`validate` and `import --dry-run` inspect configuration and source inventory
without creating artifacts or contacting services. They do not parse all records.
The launcher then calls `import --graph-manifest` for full preparation and strict
artifact verification. Expected report: `state: prepared`, `nodes: 9`, `edges: 9`.
This means storage files are prepared, not that databases have been loaded.

The launcher selects an immutable generation from input/configuration/executable
contents. Repeat the same command to verify/reuse it. Edit a valid movie CSV row
and repeat to select a new generation while preserving the old one. For another
copy of the movie source, pass `--manifest /absolute/path/to/domain.json`; paths
inside that file resolve relative to its directory. Keep source files unchanged
during each import.

Inspect the generated locations and independently invoke strict verification:

```sh
python3 - <<'PY'
import json, subprocess
from pathlib import Path
report = json.loads(Path('build/movie-onboarding.json').read_text())
print('counts:', report['counts'])
print('runtime graph declaration:', report['graph_manifest'])
print('prepared native manifest:', report['manifest'])
subprocess.run(['build/cpu-tests/bin/polygraph_movie_import', 'verify',
                '--manifest', report['manifest'], '--strict'], check=True)
PY
```

The source `domain.json` chooses configuration. The generated graph declaration
also points to the correct generation for typed root lookup. Prepared
`manifest.json` records checksums/index/storage artifacts. Pass a graph declaration
to `import --graph-manifest`; pass the prepared manifest to `verify --manifest`.
They are different files. See [manifest fields](../guides/GRAPH_MANIFESTS.md)
and [workspace isolation](../guides/GRAPH_WORKSPACES.md).

## Supply the local BFS observability configuration

The runtime currently expects `config/observability/observability.json` unless
the domain or CLI selects another file. `config/` is ignored and is not populated
by cloning. Create a minimal disabled configuration for this tutorial; the code
below preserves any existing local file:

```sh
python3 - <<'PYCONFIG'
import json
from pathlib import Path
path = Path('config/observability/observability.json')
path.parent.mkdir(parents=True, exist_ok=True)
if not path.exists():
    with path.open('x') as stream:
        json.dump({'events': {'enabled': False}, 'logging': {'enabled': False},
                   'metrics': {'enabled': False}}, stream)
PYCONFIG
```

No observability service is needed. `--no-events`/`--no-logging` disable output,
but do not bypass reading this configuration. Keep an existing configuration if
you already have one; its enabled collectors/sinks may need their own setup.

## Import into real databases and run BFS

The offline report alone cannot support distributed BFS. The following opt-in
command creates fresh isolated databases, imports the movie fixture using the
same engine, verifies stored data, then runs actual CPU BFS with typed roots:

```sh
python3 tools/ingestion/disposable_live.py --run --adapter movie-graph \
  --binary build/cpu-tests/bin/polygraph_movie_import \
  --bfs build/cpu-tests/bin/polygraph_bfs \
  --output-dir build/movie-live --unique-output
```

This step needs a running Docker daemon and cached images named
`polygraph-neo4j:5.26-enterprise`, `polygraph-arangodb:3.12`, and `postgres:17`.
The repository's [Neo4j Dockerfile](../../storage_layer/neo4j/Dockerfile) and
[ArangoDB Dockerfile](../../storage_layer/arango/Dockerfile) define the two custom
image tags through [the storage Compose file](../../storage_layer/docker-compose.yml).
Build/cache those images and cache PostgreSQL before this step, respecting
Neo4j's license requirements. Image overrides and cleanup/recovery are documented
in [the live smoke guide](../guides/MOVIE_LIVE_SMOKE.md). The movie test does not
pull images automatically. Missing prerequisites return **77 (skipped)** with
reasons; they are not a successful import. Existing project services are not used.

Expected result: `state: passed` in the printed report and the run directory's
`integration-result.json`. Native/proxy/edge counts are Neo4j **4/3/6**, ArangoDB
**3/6/9**, PostgreSQL **2/3/3**. Directed Person:1 reaches 3 vertices; undirected
Person:1 reaches 6 and Film:2 reaches all 9, through depth three. These are
independent fixture expectations, not throughput measurements.

For each root the harness invokes `polygraph_bfs --domain <generated-runtime-domain>`
with `--start-source`, `--direction`, `--max-depth` and `--output-file`. It records
the exact selectors and CSV paths. A selector is:

```json
{"namespace":"movie-graph","type":"Film","idType":"int64","id":2}
```

`Film:2` is a typed external ID, not GlobalId `2`. Canonical IDs are allocated by
the engine; do not calculate them from the external number. Runtime metadata
contains routing information, not an embedded replacement graph. The harness
removes its owned containers/network after verification and retains private
artifacts/logs. The generated runtime domain is therefore a diagnostic record,
not a connection to services that remain running.

For operator-managed targets, set `paths.targetConfig` and run `import
--graph-manifest <domain.json> --execute --confirm-create` on fresh isolated
targets. This does not provision services; Neo4j requires configured offline
import/preflight/start commands. Configure runtime `paths.backendConfig` for those
same populated stores before invoking BFS yourself. See
[native execution](../guides/BULK_INGESTION.md) and
[runtime roots](../guides/RUNTIME_GRAPH_METADATA.md).

## Use JSONL or LDBC without writing an adapter

The stock executable supports these checked-in examples offline:

```sh
build/cpu-tests/bin/polygraph_bulk_import import \
  --graph-manifest docs/examples/ingestion/logical-graph/domain.json
build/cpu-tests/bin/polygraph_bulk_import import \
  --graph-manifest docs/examples/ingestion/ldbc.domain.json
```

The first contains Device/Site/Gateway/Sensor canonical JSONL. The second is a
small LDBC SNB CSV fixture, not a full downloaded SNB dataset. For real SNB input,
copy the declaration, resolve its paths relative to the copy, select your immutable
raw snapshot with `paths.sourceData`, and use a fresh `paths.ingestionArtifacts`.
Keep a runtime data/metadata path separately in `paths.data`.

| Source | Registry name | Identity strategy | Compatibility |
| --- | --- | --- | --- |
| Canonical PolyGraph JSONL | `polygraph-jsonl` | `preserve-canonical-v2` | Existing canonical IDs/owners retained; placement cannot silently reassign them. |
| LDBC SNB CsvBasic | `ldbc-snb` | `ldbc-block31-v3` | Existing block31 IDs, auxiliary joins and parallel relationship-row semantics retained. |
| Linked movie CSV | `movie-graph` | `typed-external-v1` | Exact typed source identities; requires the movie executable. |

Old runtime-only domain files still work with runtime loaders. To opt into native
import, add `ingestion.adapter`, `paths.placementConfig` and the relevant source/
output paths; there is no automatic adapter inference or migration command.
Existing `prepare`, `execute`, JSONL tooling and `load --docker` remain available.
Do not change an existing ID strategy to migrate a populated database in place.

## Common problems

| Symptom | Check or action |
| --- | --- |
| Unknown `movie-graph` adapter | Use `polygraph_movie_import`; inspect its `adapters` output. |
| `graph.paths:` / missing CSV | All declaration paths are relative to the manifest, without shell/environment expansion. Movie CSV headers/order are fixed. |
| Dry run passes, full import fails | Inventory checks do not prove record types, uniqueness or endpoint closure. Read the full parser/strict-verification error. |
| Changed-input/output conflict | Keep old generations immutable. Use the movie launcher or a fresh native output directory; do not delete journals to force a retry. |
| BFS root missing or ambiguous | Supply exact namespace/type/`idType`/ID and the matching prepared index in `paths.ingestionArtifacts`. |
| Live command returns 77 | Read prerequisite reasons: executable, linked adapter, Docker access or cached image missing. |
| Could not open observability config | Create the minimal local config above, or select a readable file through domain `paths.observabilityConfig` / BFS `--observability-config`. |
| Live import fails | Inspect private run logs and cleanup status. Never point the fixture workflow at production stores. Use the ownership-checked recovery command in the live guide. |

See [adding a dataset](adding-a-dataset.md) for extension boundaries and
[the contract suite](../guides/ADAPTER_CONTRACT_TESTS.md) for offline checks.

## Command verification

For this guide, the three named application targets were configured and built
from a clean local checkout of commit `a18f264`, then adapter discovery, validation,
dry run, movie onboarding/reuse/strict verification, JSONL import and LDBC fixture
import were executed there. Configuration used cached spdlog/argparse source
overrides; it did not test downloading dependencies on a clean machine. System
development packages were already installed. The public HTTPS clone, package
installation and image acquisition were not rerun. Those setup steps require
network access and are prerequisites, not reported test passes.

After adding the explicit local observability setup, the documented live command
also passed from that clean checkout using cached images: stored data and all
three BFS oracles matched, owned cleanup succeeded, and 51 pre-existing containers
and 115 volumes remained unchanged. The initial missing-config failure is included
in troubleshooting above. CUDA and arbitrary external SQL schemas were not tested.
