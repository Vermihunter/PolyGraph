# Graph-aware Python tools implementation plan

**Goal:** Implement #35 under #26, consolidating graph configuration and existing
native import orchestration while preserving dataset/benchmark compatibility.

**Architecture:** Extend existing graphio.domain resolution rather than inventing
a registry/framework in Python. Generic tools accept a manifest path, graph
directory or graph ID under an explicitly selected data root. The native importer
remains authoritative. Python forwards typed BFS roots to the C++ resolver.

## Survey and scope

- `polygraph.py` already delegates native import/validation but resolves other
  domains only under repository data; eagerly imports LDBC conversion/statistics.
- `graphio.domain` already resolves domain IDs and relative paths, but assumes a
  startUuid even for import-only or typed-source manifests.
- `tools/algorithms/bfs.py` is a generic reference BFS with eager LDBC catalogs and
  reconstruction mixed into the same file. Move that compatibility code into
  ldbc_snb; retain wrapper names/CLI aliases.
- `tools/load_graph.py` has legacy endpoint/university defaults. Add explicit
  graph-manifest mode delegating to the same native import handler; preserve old
  positional loader behavior and forbid mixing its reset/connection flags.
- Benchmark suites currently resolve all paths against the repository. Add opt-in
  `pathBase: config` for portable external suites; keep repository default.
- Raw conversion, Datagen, SF-specific setup matrices and research configurations
  are explicit compatibility/support workflows, not generic onboarding. Move pure
  LDBC scale/raw-path/Datagen-parameter helpers into its support package and make
  generic imports lazy. Do not alter lifecycle/reset semantics of those scripts.

## Decisions and compatibility

- One `resolve_graph_manifest` helper selects path/directory/ID; `resolve_domain`
  loads that path. `--data-root` controls ID discovery without repository edits.
- Generic CLI commands retain positional domain aliases and also accept
  `--graph-manifest`. Conflicting selectors fail before writes. Native import
  remains `polygraph.py import`; load_graph's new manifest mode forwards there.
- DomainConfig retains numeric defaults and adds optional typed root metadata;
  absent roots are valid for import/config inspection. Run without a root gets a
  clear error. Typed roots use C++ --start-source, never Python ID allocation or
  SQLite resolution. Measurement labels hash selector JSON only for filenames;
  canonical UUID measurement paths/columns retain their existing semantics.
- `run --output-dir` permits external measurement paths; default remains existing
  measurements/<run-id>. Broader workspace/lifecycle changes are outside scope.
- Reference BFS --graph-manifest uses normalized data and numeric roots through
  the shared resolver. Typed source defaults require C++ run or an explicit
  canonical --start-uuid; do not duplicate the core source resolver.
- Opt-in config-relative benchmark paths do not change existing checked-in suites
  or composeInstance SF aliases. Generic projects can use existing composeProject.

## Tasks

- [x] Add failing config/root/script tests with external directories and spaces.
- [x] Extend shared resolver/domain metadata; wire generic CLI aliases/native
  delegation and typed-root forwarding, preserving old numeric behavior.
- [x] Move isolated LDBC helpers/reference loading; lazy compatibility imports.
- [x] Add graph-manifest modes to legacy loader/reference script and opt-in
  config-relative benchmark paths with migration errors.
- [x] Document survey, supported generic path, aliases and bounded exceptions.
- [x] Run Python, CLI/CTest/CMake/docs checks and independent review; report unrun
  live/database/GPU work. No new ingestion or root-resolution algorithm in Python.

#34 committed separately. Do not auto-commit/close #35 or start #36.

## Verification record

- Initial graph-tool tests failed on missing resolver/directory selection, CLI
  options, nullable roots, script delegates and config-relative benchmark paths.
- Review found reference config mode ignored depth/direction. A two-node script
  regression observed three failing inherited/override cases before the fix;
  all four cases now pass. Direct-input defaults remain depth 7/undirected.
- New optional-root metadata exposed unsupported benchmark defaults. Regressions
  observed a cache results sentinel being removed and a suite service hook being
  reached before root validation. Numeric root validation now precedes cache
  overwrite and suite service lifecycle; explicit existing numeric flows remain.
- `cmake --build --preset cpu-tests -j 2`: passed (CMake test registration updated;
  no production C++ changed).
- Final `ctest --test-dir build/cpu-tests --output-on-failure -j 2`: 575 passed,
  2 optional KaMinPar skips, zero failures (577 registered tests).
- `cmake --build --preset cpu-partition-tests --target polygraph_bulk_import -j 2`
  and the runtime/source/registry/partition plus Python graph-tools/import/manifest
  CTest selection: 56 passed, zero failures/skips. After final Python follow-ups,
  all three affected Python groups were rerun successfully against this build.
- Final full Python command below: 378 passed, 3297 subtests passed. An earlier
  unprivileged run had 374 passes and one socket-permission failure in existing
  `test_discovery_export_visualize_and_compare`; the final run used localhost
  permission and passed. No production fix was made for that sandbox restriction.
- CTest uses Python 3.11, while the broad Python suite used Python 3.13. CTest
  initially exposed an eager plotting/Pillow import in a config-only benchmark
  path. Plotting import is now deferred to actual suite execution, before any
  lifecycle work; config reading is independent of that optional dependency.
- New focused tests: 21 passed with the actual native importer configured,
  including fresh native aliases without LDBC support, directory/ID selection,
  typed root forwarding, legacy numeric measurement paths, CSV field selection,
  reference defaults, benchmark early errors and native reset rejection.
- `cmake --build --preset docs`, `python3 tools/docs/check_links.py` and
  `git diff --check`: passed. Independent review found no remaining issues.
- Existing CMake SQLite alias and documentation warnings remain. Live database,
  Docker lifecycle/load, Datagen and GPU execution were not run. Existing fake
  lifecycle tests and C++ ingestion/BFS regressions passed.

```sh
POLYGRAPH_BULK_IMPORT="$PWD/build/cpu-tests/bin/polygraph_bulk_import" \
  python3 -m pytest -q tools tests

ctest --test-dir build/cpu-partition-tests -R \
  '^unit.RuntimeSource|^unit.SourceIdentity|^unit.SourcePipeline|^unit.InputAdapter|^unit.IngestionPartition|^python.graph_tools|^python.import_workflow|^python.graph_manifest' \
  --output-on-failure
ctest --test-dir build/cpu-partition-tests -R \
  '^python.graph_tools|^python.import_workflow|^python.graph_manifest' --output-on-failure
```

## Compatibility decisions and limits

Explicit manifest-mode load aliases share the native import handler and default
to offline preparation. Positional legacy loaders and named LDBC/University/SF
workflows retain their behavior. Optional LDBC catalogs and plotting dependencies
are not required to select/inspect a generic graph. Pure adapter helpers and raw
reference reconstruction moved with callable wrappers retained.

`DomainConfig.start_uuid` can now be None for typed/import-only manifests; callers
must choose an explicit root or support the additive start_source metadata. No
new core ingestion, source-ID encoding/allocation or SQLite source resolver was
implemented in Python. Numeric measurement layout is preserved; typed measurement
labels are only filesystem hashes. Generic root CSV defaults to external_id;
LDBC callers explicitly request ldbc_id and retain their old output.

Suite pathBase defaults to repository; config-relative paths are opt-in. Explicit
prepare.command argv still uses the existing repository cwd. Typed roots are
forwarded by `run`; reference/suite/cache benchmarks require canonical IDs and
return an explicit native-run replacement before service or output overwrite.
Dataset/research setup scripts remain named support scenarios, documented in
GRAPH_TOOLS.md. Broader workspace isolation and lifecycle redesign were excluded.
