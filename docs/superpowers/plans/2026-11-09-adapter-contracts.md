# Adapter contract tests implementation plan (#39)

> **For agentic workers:** Use superpowers:executing-plans task by task.

**Goal:** Give future adapters one reusable offline contract suite for logical records, identity, orientation, determinism and error handling.
**Architecture:** A Python standard-library harness invokes the existing native prepare/verify CLI and reads normalized SQLite read-only. Declarative fixture/oracle/mutation files select adapters and source formats; assertions contain no dataset type/column branches. Run LDBC CSV, equivalent canonical JSONL, unfamiliar mixed-schema JSONL and typed movie CSV through that same harness. Preserve adapter-specific duplicate policy explicitly (LDBC duplicate edge rows become distinct parallel edges; repeated canonical/typed edge IDs are rejected).
**Tech stack:** Existing native executables, unittest/CTest, SQLite, static JSON fixture declarations.
**Spec:** GitHub #39, parent #27; #40 live onboarding is separate and not started.

## Constraints and review focus

- No running databases, lifecycle helpers or production-code changes unless a confirmed contract violation requires a focused fix.
- Assert exact property value types: Python bool/int equality must not hide coercion.
- Preserve canonical IDs and exact typed source keys; assert unique node/edge identities, endpoint closure and owner routing.
- Compare logical records, ID maps and backend artifact checksums across fresh directories and reruns, ignoring timing/RSS fields.
- Malformed input, duplicate identity and missing-reference failures must not publish a prepared manifest or modify sources.
- Prove the assertions reject changed types, missing columns, property coercion and reversed edge endpoints.

## Tasks

- [x] Declare four cases and independent expected records/traversal layers, with source-format mutations kept in data.
- [x] Add a failing test driver for the reusable assertions, then implement the harness and run all cases.
- [x] Register the suite in normal CMake/CTest; document how another linked adapter supplies a case, including optional example-disabled builds.
- [x] Run focused and full CPU/Python checks, docs/link/format checks and independent review. Report exact counts and unexecuted areas.

## Executed verification

- Shared contract suite: 36 tests passed in about two seconds, including strict verification after fresh output, reruns and preserved LDBC duplicate edges.
- Without the movie importer environment variable: 27 tests passed and 9 explicitly skipped.
- CPU build and full CTest: 596 passed, 3 optional/live skips (599 registered).
- Full Python suite with both built executables supplied: 436 passed, 3,365 subtests passed.
- Docs build, Markdown links, new Python formatting/static-name checks and diff whitespace checks passed.
- Review caught missing rerun edge/artifact assertions; these were added and the reviewer confirmed no remaining actionable findings.

All new tests are offline. Live database/CUDA checks and issue #40 were not started; no production implementation changed.
