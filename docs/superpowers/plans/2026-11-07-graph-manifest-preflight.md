# Graph manifest and preflight implementation plan

**Goal:** Implement only #31, using `polygraph.domain` version 1 and the existing
native adapter/prepare engine. Continue on the existing feature branch.

**Design:** Add an optional `ingestion` section selecting an adapter, an identity
strategy and native preparation options. Existing `paths.schema`, `data`,
`backendConfig` retain their runtime meanings; `sourceData` optionally selects
raw source input, and `placementConfig`, `ingestionArtifacts`, `targetConfig`,
`partitionFile`/`partitionMap` supply native paths. Resolve manifest paths relative
to the manifest directory. Native validation requires the additive ingestion
configuration; old runtime manifests and old native CLI flags remain supported.

Expose `validate --graph-manifest FILE [--execution]` and
`prepare --graph-manifest FILE`; reject ambiguous mixtures with native option
overrides. Python `tools/polygraph.py validate` delegates to the same native
validator without building or provisioning anything.

Preflight reads configuration and inventories, checks schema/placement/ID layout,
output access via existing ancestors (no write probes), and optional executor
capabilities/target config/environment/runner executability. It never creates a
scratch database, invokes an operator command, connects to services, or writes a
report. Full record uniqueness, endpoint closure, target freshness/connectivity
and actual imports remain prepare/execute checks, explicitly reported as deferred.

## Steps

- [x] Add failing unit and CLI tests for path resolution, legacy compatibility,
  manifest preparation, strict versions/fields, mapping/identity errors, missing
  inputs, inaccessible output, execution prerequisites and zero side effects.
- [x] Add manifest loading and read-only validation using existing option,
  placement, registry inventory and target configuration validation. Add optional
  adapter inspection for bounded leading metadata; no new ingestion pipeline.
- [x] Wire native validate/prepare and Python validation delegation. Keep existing
  runtime loaders, native flags, backend writers and BFS behavior intact.
- [x] Add manifest examples and document schema, defaults, error codes, path
  semantics, validation limits and additive migration from old `domain.json`.
- [x] Run unit/integration/regression, CPU/KaMinPar, Python, docs and formatting
  checks. Review independently and address material findings.

## Review focus

- Validation must leave directory/file contents and mtimes unchanged, on success
  and failure, and never run even a nominally read-only operator preflight command.
- IDs/type names/schema mappings must not be silently coerced or overridden.
- Relative manifest paths must be independent of invocation working directory.
- `paths.data` must remain runtime data when `paths.sourceData` selects raw input.
- An output path can be checked without creating its missing ancestors; report
  that filesystem writability may still change before actual preparation.

No automatic commit, push, issue closure, #32 normalization or #33 import workflow.

## Verification record

- `cmake --build --preset cpu-tests -j 2`: passed.
- `ctest --test-dir build/cpu-tests --output-on-failure -j 2`: 544 passed,
  one optional KaMinPar test skipped in the CPU preset; used localhost socket
  access for the existing Redis fake server. No live integration tests enabled.
- `cmake --build --preset cpu-partition-tests --target polygraph_graph_manifest_test polygraph_input_adapter_registry_test polygraph_ingestion_partition_test -j 2`:
  passed. `ctest --test-dir build/cpu-partition-tests -R '^unit.GraphManifest|^unit.InputAdapter|^unit.IngestionPartition' --output-on-failure`:
  all 35 passed, including real KaMinPar and registered-adapter replay.
- With `POLYGRAPH_BULK_IMPORT` pointing to the CPU importer,
  `python3 -m pytest -q tools/graphio tools/ingestion tests/reference tests/test_ldbc_snb_conversion.py tests/test_ldbc_fast_prepare.py tests/test_reference_bfs_ldbc_csv.py tools/ldbc_snb/test_statistics.py tools/ldbc_snb/test_placement_report.py tools/test_polygraph_gui.py tools/test_polygraph_instances.py`:
  209 passed (3257 subtests passed).
- `cmake --preset docs`, `cmake --build --preset docs`,
  `python3 tools/docs/check_links.py`, `git diff --check` and clang-format
  `--dry-run --Werror` on changed C++ files: passed.
- Existing environment warnings: Doxygen ignores three unsupported configuration
  tags; the partition build's conda libcurl reports missing version information.
  Builds/tests completed successfully; no new compiler warnings observed.
- Independent review found missing shared Arango option checks, Arango node/edge
  collection collision checks, and Python import bytecode writes. Their regressions
  were observed failing, fixes reuse existing behavior, and follow-up review found
  no remaining material issues. Read-only tests compare file contents/mtimes and
  directory mtimes including the workspace root, on success and failure.

Live database imports, GPU execution and the executor-disabled build configuration
were not run. Read-only execution prerequisites were tested with unreachable
endpoints and an operator command that would create a marker if invoked; it was
never invoked. Full records and live freshness/connectivity remain explicitly
deferred to existing prepare/execute paths.
