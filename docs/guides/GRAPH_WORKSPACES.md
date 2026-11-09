# Graph workspaces

Each graph config selects a deterministic artifact namespace. By default it is
`<manifest-directory>/artifacts/<graph-id>`, using the manifest's `id`. Add this
optional section to an existing `polygraph.domain` version 1 manifest to choose
a shared output base or a stable filesystem ID:

```json
{
  "workspace": {
    "root": "../generated",
    "id": "inventory-a"
  }
}
```

`root` is relative to the manifest, or absolute. `id` defaults to the graph ID;
it must start with an ASCII letter or digit and contain only letters, digits,
`.`, `_`, or `-` (maximum 128 characters). Invalid names fail rather than silently
colliding after sanitization. A graph name for display can remain descriptive.

```text
generated/
  inventory-a/
    ingestion/       native manifest, generations, source-ID index, import files
    caches/          named cache plans from cache build
    cache/           single plan from cache reconfigure
    index/           reusable cache-plan.sqlite3
    gpu-storage/     authoritative GPU placement artifacts
    measurements/    run and cache-comparison results
  inventory-b/
    ...
```

No workspace resolution or native `validate` command creates directories.
Preparation creates its namespace only when it writes artifacts. Runtime typed
root lookup uses the same default `ingestion/manifest.json` as native preparation.

## Selecting and rebuilding a graph

```sh
python3 tools/polygraph.py validate --graph-manifest /graphs/a/domain.json
python3 tools/polygraph.py import --graph-manifest /graphs/a/domain.json
python3 tools/polygraph.py cache build --graph-manifest /graphs/a/domain.json \
  --config /configs/cache-small.json
python3 tools/polygraph.py run --graph-manifest /graphs/a/domain.json
```

These commands select one config. Preparing another config with a different ID
uses another directory. Native ingestion retains its existing fingerprint and
exclusive directory lock; an identical rerun verifies/reuses its artifacts, and
changed inputs/configuration require a fresh ingestion directory or generation.
Cache rebuilding for unchanged source identity may regenerate only that graph's
owned artifacts and SQLite index. Changing graph contents at the same input path
invalidates the old cache index signature and rebuilds it.

Workspace reservation files beside namespace roots bind the root to its selecting
manifest and normalized graph path. Directory ownership files bind cache/run
outputs to that same config; artifact/index sidecars also record the graph input.
Conflicting configs, ancestor aliases, symlink descendants and foreign indexes
fail before their artifacts are overwritten. Different dataset names that would
collapse into one benchmark-suite output directory are rejected too. These are
filesystem isolation checks, not simultaneous multitenant runtime support.

## Explicit paths and migration

Existing `paths.ingestionArtifacts`, `--output-dir`, `--cache-root`, and
`--planning-db` remain supported. Native explicit directories retain their
fingerprint validation. Python overrides are claimed at the requested path, so
using one graph's destination for another fails. Existing `cachePlan` and
`gpuStoragePlan` references remain readable; no files are moved automatically.

Old nonempty Python output directories without ownership metadata are left
untouched. Select a fresh output directory to rebuild them. A legacy SQLite
planning index can be reused only if its stored `graph.path` proves that it belongs
to the selected graph. Unknown, corrupt or foreign indexes are not deleted.
Standalone cache/GPU planner calls that have no domain manifest default to
`<graph-directory>/.polygraph/<path-hash>/index/cache-plan.sqlite3`; two input
files in the same directory therefore have separate indexes. The hash is an
artifact namespace, never a graph ID.

Legacy LDBC preparation keeps its standard default directory. A custom
`--domain-id` gets a distinct `graphs/<id>` subdirectory unless `--output-dir` is
explicit. Legacy preparation records source ownership before replacing generated
files; existing unowned outputs need a fresh directory. University preparation
keeps its single built-in domain directory with the same source checks. Generated
benchmark configs now live beside their prepared graph under `benchmark-configs/`
rather than replacing repository-wide config files.

Relocating a selecting manifest or changing its normalized graph path changes its
ownership identity. Use a fresh workspace root/ID or explicit fresh output paths;
this change does not rewrite existing ownership metadata or migrate deployments.

## Removing selected artifacts

There is no new cleanup command. Stop writers for the chosen graph, inspect its
resolved workspace, and remove only the generated subdirectories you intend to
rebuild (`ingestion`, `cache`, `caches`, `index`, `gpu-storage`, or `measurements`).
Leaving the adjacent namespace reservation preserves the graph's identity for its
next rebuild. Do not remove source/config files or edit ownership files to claim
another graph's output.

The existing LDBC benchmark-plan helper's `--clean` validates the selected graph's
cache/index ownership before removing those artifacts. It does not remove graph
sources, database containers, bind-mounted database data or Docker volumes.
Explicit benchmark `--overwrite` similarly validates ownership and protected
inputs before removing its selected measurement directory.

## Verification for #37

Native and Python tests prepare two configs, compare the second graph's files
before and after rebuilding the first, reject shared directory/index ownership,
check read-only validation and runtime lookup, and exercise guarded benchmark
replacement and scoped legacy cache cleanup. Source/config symlink and temporary
output regressions run before writers are invoked.

Executed checks:

- `cmake --build --preset cpu-tests -j2`: passed.
- `ctest --test-dir build/cpu-tests --output-on-failure -j2`: 589 passed; two optional KaMinPar tests and the opt-in live backend test skipped (592 registered).
- Full `python3 -m pytest -q tools tests` with `POLYGRAPH_BULK_IMPORT` set: 396 passed, with 3,328 subtests passed.
- Docs build, local links, C++ formatting, new Python module formatting and `git diff --check`: passed.

Live database and CUDA execution were not rerun for this filesystem change.
No database containers or volumes were changed by these checks.
