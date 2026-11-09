# Graph onboarding documentation plan (#41)

Use the executing-plans skill for this focused documentation task.

**Goal:** Copyable configuration-first onboarding and linked-adapter development, grounded in the implemented CLI and existing ingestion engine. Parent: #27. No new runtime or importer features.

- [x] Inspect manifests, CLI, adapter interfaces, tests, build prerequisites and existing guides; check repository instructions.
- [x] Write `docs/graphs/quickstart.md`: clone/build, offline validation/import/inspection, movie live import/BFS, JSONL and LDBC compatibility, services and troubleshooting.
- [x] Write `docs/graphs/adding-a-dataset.md`: configuration decision, architecture/directory layout, exact reader/registry composition, identity/edge rules and shared contracts; distinguish existing SQL schemas.
- [x] Link both guides from README/wiki and correct stale cross-references in directly relevant guides.
- [x] Test documented build/offline commands in a clean source checkout, run the documented live command, contract/CLI tests, docs build and link checks. Record dependency-cache use and unverified setup steps.
- [x] Review requirements and commands, resolve concrete findings, complete the user-authorized commit/push/closure without starting another issue.

## Evidence and findings

- #40 committed/pushed as `a18f264` and closed before starting this issue.
- Clean source clone: `build/issue41-checkout`; fresh build of all three documented application targets succeeded (117 build steps). Configure supplied `FETCHCONTENT_SOURCE_DIR_SPDLOG` and `FETCHCONTENT_SOURCE_DIR_ARGPARSE` from existing dependency source cache; system development libraries already installed. HTTPS clone, package installation and image acquisition were not rerun.
- Clean-checkout movie discovery, validate, dry-run, onboarding, strict verify, unchanged-input reuse, JSONL fixture import and LDBC fixture import executed successfully.
- First clean-checkout live attempt verified the real import but failed BFS: ignored local observability config was absent. Added a copyable non-overwriting minimal disabled config step and troubleshooting; repeated config command preserves existing bytes/mtime. No runtime behavior was changed to hide the prerequisite.
- Current workspace application build and 7 focused CTests passed; Python movie/adapter/manifest suite: 49 passed and 39 subtests. Documentation build and local-link checks passed.
- Independent review checked exact interfaces/commands/identity rules and the observability prerequisite; no remaining findings.

- Corrected clean-checkout live run passed: `build/issue41-checkout/build/movie-live/run-2bd8cc9eedba/integration-result.json`. All three stores and BFS oracles verified; cleanup removed owned resources; 51 pre-existing containers and 115 volumes unchanged. User authorized committing and closing #41 after review; no later issue started.
