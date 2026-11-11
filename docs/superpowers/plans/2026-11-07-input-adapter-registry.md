# Input adapter registry implementation plan

**Goal:** Implement issue #30, the first part of Phase 2 (#25), using the existing streaming/native ingestion engine.

**Spec:** [Logical graph contract](../../architecture/logical-graph-contract.md).

**Design:** Keep `IGraphInputAdapter` and the existing `prepare()` signature. Add
an explicitly populated registry of statically linked factories, input inventory
callbacks and an identity-policy selector. A caller can select a local registry
through an optional non-owning `PrepareOptions` member; the CLI uses an immutable
built-in registry. No global mutation, dynamic loading or alternate pipeline.
Keep both built-in input fingerprints and ID strategies unchanged. New adapters
emit canonical IDs until #32 provides generic source-ID allocation.

**Files:** public registry header, registry implementation and built-in factory
registration; small changes to Import/Input/Normalize and CLI selection; existing
ingestion CMake targets, a focused C++ test target, Python CLI regressions and docs.
Backend writers, database schemas and BFS algorithms remain unchanged.

## Steps

- [x] Write failing C++ tests for discovery, invalid/duplicate registration,
  factory errors, synchronous streaming/consumer cancellation and a test adapter
  preparing all three native artifact formats with strict verification.
- [x] Implement the registry and route creation, inventory and ID-policy lookup
  through it. Fingerprint nonempty adapter versions without changing legacy
  fingerprints. Reject ID-less records from canonical adapters explicitly.
- [x] Add CLI discovery and precise unknown-adapter diagnostics. Remove the
  Docker helper's duplicate hardcoded adapter-name filter; native preparation
  validates selection before provisioning services.
- [x] Verify both built-ins, partition replay, custom inventory/version recovery
  invalidation and streaming failures. Document static registration, metadata,
  lifetime and current ID limitations using the public API.
- [x] Run affected build/tests, CPU CTest suite, Python checks, docs/link checks,
  then request an independent review and address material findings.

## Review focus

- An unsupported adapter must fail before source inventory or factory work.
- Registration must not silently replace a built-in or accept missing callbacks.
- Custom inventory must participate in both initial and post-stream identity checks.
- Consumer errors must stop streaming immediately and prevent manifest publication.
- Existing LDBC/JSONL checkpoints, partition projection and ID ownership must survive.

No automatic commit, push, issue closure or work on #31/#32 is part of this plan.

## Verification record

- `cmake --build --preset cpu-tests -j 2`: passed.
- `ctest --test-dir build/cpu-tests --output-on-failure -j 2`: 531 passed,
  one optional KaMinPar test skipped (this preset has KaMinPar disabled).
  Run with localhost socket access for the existing Redis fake-server test.
- `cmake --build --preset cpu-partition-tests --target polygraph_input_adapter_registry_test polygraph_ingestion_partition_test -j 2`:
  passed; `ctest --test-dir build/cpu-partition-tests -R '^unit.InputAdapter|^unit.IngestionPartition' --output-on-failure`:
  all 24 passed, including the custom adapter's topology/normalization replay.
- With `POLYGRAPH_BULK_IMPORT` pointing to the built CPU importer,
  `python3 -m pytest -q tools/graphio tools/ingestion tests/reference tests/test_ldbc_snb_conversion.py tests/test_ldbc_fast_prepare.py tests/test_reference_bfs_ldbc_csv.py tools/ldbc_snb/test_statistics.py tools/ldbc_snb/test_placement_report.py`:
  193 passed (3255 subtests passed).
- `cmake --preset docs`, `cmake --build --preset docs`,
  `python3 tools/docs/check_links.py`, `git diff --check` and clang-format's
  `--dry-run --Werror` on changed C++ files: passed. Installed Doxygen ignores
  three existing unsupported configuration tags.
- Reused the pre-registry canonical artifact from issue #28's temporary example
  with an unchanged generation/input identity. Built-in identity shape and both
  ID strategies are also covered by a permanent regression test.
- Independent review identified unversioned custom registration as a checkpoint
  risk. Observed its regression fail, rejected empty custom versions, retained a
  private legacy-only registration path, and verified policy/version invalidation.
  Follow-up review found no remaining actionable issues.

No live Docker/database import or GPU execution was run. The three database
artifact writers and strict offline verification were exercised.
