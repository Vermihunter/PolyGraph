# Isolated movie onboarding and BFS smoke plan (#40)

> **For agentic workers:** Use superpowers:executing-plans task by task.

**Goal:** One explicit opt-in command prepares and imports movie CSV into fresh real Neo4j/ArangoDB/PostgreSQL services, verifies stored data and runs the independent directed/undirected BFS oracle.
**Architecture:** Extend `tools/ingestion/disposable_live.py` with a native movie fixture path. Reuse `DockerEnvironment`, shared generic import, native live verification, runtime bootstrap and cancellation/logging helpers. A test-side graph verifier reads expected logical records from normalized SQLite and queries real backends for nodes, replicas, proxies, properties and endpoint handles. Verify the fixture's hand-authored reachability with the actual C++ BFS and typed source roots. No new importer or deployment manager.
**Tech stack:** Existing C++ binaries, Python standard library, Docker CLI/ownership helpers, backend read-only APIs, unittest/CTest.
**Spec:** GitHub #40, parent #27. #41 documentation rollout is not started.

## Constraints and review focus

- `--run` is mandatory; check executable/adapter/Docker/cached-image prerequisites before provisioning. Missing prerequisites report skipped and return 77.
- Use fresh uniquely named owned containers/network and bind-mounted directories only. Snapshot pre-existing container/volume metadata; never mutate those resources.
- Build execution targets and runtime connections solely from the newly owned environment, overriding any source-config targets.
- Report success only after generic import, native live verification, actual stored attributes/endpoints and fixture BFS pass, and cleanup succeeds.
- Always attempt ownership-checked cleanup on failure/cancellation; retain private logs/artifacts and record cleanup errors transparently.
- Fix the confirmed random-password leading-dash Neo4j startup failure with an alphanumeric prefix and regression test.

## Tasks

- [x] Add failing service-free tests for password safety, opt-in/prerequisite behavior, canonical backend read normalization, mismatch detection and cleanup/reporting.
- [x] Implement the movie extension and reusable test-side live graph assertions using the existing lifecycle and import workflow.
- [x] Run the explicit command against available cached real services; inspect actual import/data/BFS reports and cleanup, with skipped prerequisites reported honestly.
- [x] Register opt-in CTest and document one-command use, retained artifacts, skip codes and ownership boundaries; run relevant full CPU/Python/docs checks and review.

## Verification evidence

- Real movie import verified all three stores: Neo4j 4 native nodes / 3 proxies / 6 relationships, ArangoDB 3 / 6 / 9, PostgreSQL 2 / 3 / 3. User properties, physical/logical types and endpoints matched normalized records.
- Actual BFS matched independent layer oracles: undirected Person:1 → 6 vertices, undirected Film:2 → 9, directed Person:1 → 3.
- Successful runs `build/issue40-live/run-c7e6af139030/integration-result.json` and `build/issue40-live/run-56cfa1851875/integration-result.json`: owned cleanup succeeded; 51 existing containers and 115 existing volumes unchanged.
- CPU suite: 600 registered, three optional tests skipped; no failures. Python suite: 446 passed plus 3371 subtests before the final additional signal-during-cleanup regression; final targeted tests executed separately.
- Docs build, local Markdown links, Python formatting/undefined-name checks and whitespace checks passed.
- Review found cancellation-reporting and cleanup shielding gaps; fixed with explicit interruption handling, cleanup signal shielding and regression coverage. Docker mount metadata order is canonicalized to prevent false preservation failures.
- No core/backend implementation or ingestion framework changed. #41 is not started. Live coverage uses the small movie fixture and cached local image versions; it does not certify arbitrary versions or large-scale performance.
