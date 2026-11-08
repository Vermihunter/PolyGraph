# Native graph import workflow implementation plan

**Goal:** Implement #33: a single manifest-driven import entry point, preserving
legacy native/Python and disposable Docker workflows.

**Spec:** GitHub #33 under #25, existing graph manifest and logical graph contract.
**Architecture:** Add `polygraph_bulk_import import --graph-manifest <domain.json>`
using the existing native `load_graph_manifest`, `validate_graph_manifest`,
`prepare`, `verify`, and `execute`. Python `tools/polygraph.py import` forwards
argv to that binary; it never selects parsers, writes artifacts or provisions
services. C++ is the authoritative ingestion/execution engine; Python's existing
Docker lifecycle helper remains the owner of disposable resources.
**Tech stack:** Existing C++ CLI, Python argparse/subprocess, CTest/unittest.

## Decisions and constraints

- Default is prepare-only, including strict artifact verification. It writes only
  declared ingestion artifacts and never contacts a database or runs commands.
- `--dry-run` performs read-only manifest preflight without preparing records;
  it is not a record-integrity or live-freshness check.
- `--execute --confirm-create` opts into existing native fresh-target execution,
  with target configuration from `paths.targetConfig`. All offline execution
  prerequisites are checked before preparation. `--concurrency` applies only to
  execution and must be in [1,64]. Conflicting/unsafe flags fail before writes.
- No source-specific branches, backend format changes, reset flags, service
  lifecycle, image pulling, implicit retries or verification bypass in the new
  command. Existing `load --docker`, legacy prepare/execute and Python domain
  commands remain usable; no auto-migration of their semantics.
- Return JSON state, graph, adapter, output/manifest paths, counts and per-backend
  counts on success. Errors include adapter (when resolved), phase and original
  source diagnostics; native executor failures add backend context.
- Alternative: a Python orchestration engine would duplicate normalization and
  safety policy. Replacing legacy `load` would break users. Additive CLI wiring
  is the focused extension supported by the existing architecture.

## Tasks and review focus

- [x] Add CLI integration regressions failing on missing `import` command:
  LDBC/JSONL default preparation, strict verify, readonly dry run and service traps,
  malformed source diagnostics, unsafe flag combinations and Python delegation.
- [x] Wire native import modes/reporting and Python forwarding without implicit
  build/provision. Preserve child diagnostics and native exit status.
- [x] Add backend/phase context to native preflight/import/verify errors, covered
  by existing injectable executor unit tests and CLI refusal regression.
- [x] Document one entry point and precise dry-run/default/execution behavior.
- [x] Build/run CPU, relevant KaMinPar/Python regressions, docs/links/format checks;
  obtain independent review and report live/GPU checks not executed.

Review focus: execution without consent; dry run on fresh checkout; source/target
errors before artifact writes; backend preflight before imports; legacy Docker
owned-resource cleanup and no implicit retries.

#32 is committed separately. Do not automatically commit/close #33 or start #34.

## Verification record

- Red phase: the new workflow tests failed because native/Python `import` was
  absent. The executor-context test failed for all three injected phases before
  contextual wrapping was added.
- `cmake --build --preset cpu-tests -j 2`: passed.
- `ctest --test-dir build/cpu-tests --output-on-failure -j 2`: 561 passed,
  2 optional KaMinPar skips, zero failures (563 tests). Includes existing BFS,
  LDBC/JSONL, fake-Docker lifecycle/safety and new import workflow regressions.
- `cmake --build --preset cpu-partition-tests --target polygraph_bulk_import
  polygraph_ingestion_test polygraph_ingestion_partition_test
  polygraph_input_adapter_registry_test polygraph_source_identity_test -j 2`:
  passed.
- `ctest --test-dir build/cpu-partition-tests -R
  '^unit.SourceIdentity|^unit.SourcePipeline|^unit.InputAdapter|^unit.Ingestion|^python.import_workflow|^python.graph_manifest'
  --output-on-failure`: 79 passed, zero failures/skips. Includes real KaMinPar
  and the new CLI against that build.
- Python regression command below: 219 passed, 3268 subtests passed.
- `cmake --build --preset docs`, `python3 tools/docs/check_links.py`,
  `git diff --check`, clang-format dry-run on all changed C++: passed.
- Independent read-only review found no blocking issues and reran all 10 workflow
  CLI tests successfully. Execution success through the new wrapper is covered
  indirectly by existing injectable-executor tests; no live database import was
  performed. Confirmed CLI execution was exercised through native PostgreSQL
  preflight refusal using a nonexistent local socket, preserving backend/phase
  diagnostics and native exit status through Python.
- The earlier usage-limit interruption prevented one approval review and one
  independent review attempt from running. After the user requested continuation,
  approval retry and independent review succeeded; no check was bypassed.
- Existing Doxygen unsupported-option and Conda libcurl version-information
  warnings remain. Live database/Docker imports and GPU execution were not run.

```sh
POLYGRAPH_BULK_IMPORT="$PWD/build/cpu-tests/bin/polygraph_bulk_import" \
  python3 -m pytest -q tools/graphio tools/ingestion tests/reference \
  tests/test_ldbc_snb_conversion.py tests/test_ldbc_fast_prepare.py \
  tests/test_reference_bfs_ldbc_csv.py tools/ldbc_snb/test_statistics.py \
  tools/ldbc_snb/test_placement_report.py tools/test_polygraph_gui.py \
  tools/test_polygraph_instances.py
```

## Acceptance review

- One documented native/Python command prepares both built-in adapters with all
  three existing writers; the manifest supplies selection/configuration.
- Default preparation is offline. Dry run leaves even a fresh Python checkout
  unchanged. Lifecycle-command traps remain untouched, and a pre-existing data
  sentinel retains its content and mtime.
- Invalid mode combinations, missing consent and missing execution targets fail
  before artifact creation. Source errors retain file/record context. Native
  executor failures include backend and preflight/import/verify phase.
- Legacy native commands, Python domain commands and owned-resource Docker load
  were retained; their existing regression suites pass. No backend formats,
  database schemas, lifecycle cleanup rules or retry policies changed.
