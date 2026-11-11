# Movie graph example implementation plan (#38)

> **For agentic workers:** Use superpowers:executing-plans task by task.

**Goal:** Onboard Person/Film/Studio CSV data with reused typed IDs through the existing ingestion engine and prove 1–3-hop reachability.
**Architecture:** A statically linked example adapter emits typed source identities and properties. Expose the existing bulk-import CLI as a registry-accepting entry point (the current executable hardwires built-ins despite the engine's injectable registry). A tiny example composition root adds the reader; no movie-specific case enters generic code, normalization or writers. An example launcher hashes immutable inputs/config/executable into generation-specific manifests and invokes the same shared import command, permitting edits with one repeated command while preserving prior generations.
**Tech stack:** C++23, standard CSV reader scoped to the example, existing native import/SQLite index, Python standard-library launcher, GTest/unittest/CTest.
**Spec:** GitHub #38, parent #27. #39 contract expansion and #40 live onboarding are separate issues, not started here.

## Constraints and review focus

- Built-in registry and existing CLI/error codes stay compatible.
- CSV quoting, malformed typed values and reused int64 IDs across entity kinds/types must be handled explicitly.
- Adapter streams synchronously in two phases; no private ingestion headers or extra graph store.
- Every generation binds CSV/config contents and executable identity; no cleanup, database reset, or source rewrite.
- Oracle uses independently specified typed source IDs and both directed and undirected semantics.

## Tasks

- [x] Add failing CLI registry seam and movie adapter tests; record the failures.
- [x] Extract the unchanged CLI implementation behind `run_bulk_import_cli(..., registry)`; keep its original main and add an example composition root.
- [x] Implement six CSV table mappings, typed IDs/properties and fixed input inventory solely under `examples/movie-graph`.
- [x] Add fixtures, graph/placement/backend manifests and an independent reachability oracle; add the content-generation launcher using the shared import command.
- [x] Verify CSV modification/reimport, native normalization/strict artifacts and 1–3-hop BFS oracle, including all three backend writer formats. Run relevant full CPU/Python/docs checks and independent review.

## Contract finding

The CLI registry gap was resolved only at the application composition boundary. `app/bulk_import/BulkImportCli.cpp` retains the existing commands and accepts a registry; the stock main passes defaults, and the example main passes its linked reader. Ingestion core, generic scripts and backend writers are unchanged.

## Verification

- Focused CSV/CLI/example tests: 6 passed, including the Python example module's 4 cases.
- Full CPU build passed; CTest: 595 passed, 3 optional/live skips (598 registered).
- Full Python suite with both importer paths supplied: 400 passed, 3,331 subtests passed.
- Documented onboarding command executed: 9 nodes, 9 edges, prepared and strictly verified.
- Docs build, local links, C++/new Python formatting and Python 3.11 syntax checks passed.
- Independent read-only review: no actionable findings.

Live database and CUDA execution were not run; #40 remains a separate issue. No production ingestion-core, generic Python script or backend-writer code changed.
