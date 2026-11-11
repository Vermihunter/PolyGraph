# Source identities implementation plan

**Goal:** Implement #32 inside the existing SQLite-backed ingestion pipeline.

**Architecture:** Add optional structured source identities to `Record` and a
`typed_external` registry policy. Encode namespace, implicit vertex/edge kind,
logical type, exact ID value type and value as an injective JSON-array key. Use
that same key in normalization and partition projection. Allocate global payloads
in stream order after placement, without storing all identities in memory.
Retain canonical JSONL and legacy LDBC paths and fingerprints.

**Spec:** GitHub #32 and `docs/architecture/logical-graph-contract.md`.
**Tech stack:** Existing C++20, SQLite, GoogleTest, CMake, Python regression suite.

## Constraints and decisions

- Extend existing `Record`, registry and SQLite tables; no new ingestion engine.
- Error on duplicate source/canonical identities and missing endpoints; no skip,
  dangling-node creation or upsert policy is defined by the contract.
- Namespace and logical type are explicit. Signed integers, unsigned integers and
  strings remain distinct. Edge identities are optional; parallel ID-less edges
  receive distinct stream-order IDs. Canonical IDs, when supplied, are preserved;
  generated/explicit collisions fail rather than silently changing an ID.
- Generated IDs are deterministic for identical order, configuration and versions,
  not stable across reordered records. SQLite retains source-to-global mappings.
- Legacy opaque keys remain unchanged. Typed policy uses structured identities
  exclusively and rejects conflicting opaque keys/references.
- Alternatives: pre-encode tuples in every adapter duplicates validation and makes
  partitioning inconsistent; in-memory resolution violates issue constraints.
  Shared structured key helpers and SQLite are the focused extension.

## Review focus

Namespace/type/kind separation; integer boundaries and delimiter-rich strings;
forward endpoints and duplicate explicit edges; partition replay and ownership;
large streams with bounded peak RSS and persistent mapping.

## Tasks

- [x] Add failing tests for the public identity encoding and typed pipeline API.
- [x] Implement optional source identities, shared key helpers and typed allocation;
  use them for SQLite endpoints and partition projection. Share strategy naming
  across graph validation, generation identity and prepared manifests.
- [x] Exercise duplicate/missing/invalid/conflicting identities, all three writers,
  deterministic recovery, external partitions and KaMinPar replay.
- [x] Add a subprocess large-stream normalization/peak-RSS regression.
- [x] Update contract and adapter documentation, run CPU/KaMinPar/Python/docs checks,
  inspect diff and obtain independent review. Report unexecuted live/GPU checks.

No automatic commit or issue closure for #32 without a further request.

## Verification record

- `cmake --build --preset cpu-tests -j 2`: passed.
- `ctest --test-dir build/cpu-tests --output-on-failure -j 2`: 559 passed,
  2 optional KaMinPar skips, zero failures (561 registered tests).
- `cmake --build --preset cpu-partition-tests --target
  polygraph_source_identity_test polygraph_input_adapter_registry_test
  polygraph_ingestion_partition_test -j 2`: passed.
- `ctest --test-dir build/cpu-partition-tests -R
  '^unit.SourceIdentity|^unit.SourcePipeline|^unit.InputAdapter|^unit.IngestionPartition'
  --output-on-failure`: 40 passed, zero skips/failures. Includes real KaMinPar
  structured-identity replay and legacy adapter partition compatibility.
- Python graphio/ingestion/reference/LDBC/GUI/instances regression selection:
  209 passed, 3257 subtests passed. Full command recorded below.
- `cmake --build --preset docs`, `python3 tools/docs/check_links.py`,
  clang-format dry-run on changed C++, and `git diff --check`: passed.
- Independent review found no remaining correctness issues. Two follow-up
  regressions were observed failing before the fixes: canonical collisions lacked
  context and new typed tables altered legacy schema. Both now pass.
- Large-stream regression normalizes 200,000 vertices (512-byte property each)
  and 199,999 edges in a subprocess, verifies persisted endpoint joins and >100 MiB
  disk index, and asserts peak RSS below 80 MiB with a 16 MiB engine budget.
- Existing Doxygen unsupported-option and Conda libcurl version-information
  warnings persist. Live database imports, Docker lifecycle, GPU execution and an
  executor-disabled build were not run. Backend artifact strict verification and
  existing BFS contract/concurrency tests passed in the CPU suite.

```sh
POLYGRAPH_BULK_IMPORT="$PWD/build/cpu-tests/bin/polygraph_bulk_import" \
  python3 -m pytest -q tools/graphio tools/ingestion tests/reference \
  tests/test_ldbc_snb_conversion.py tests/test_ldbc_fast_prepare.py \
  tests/test_reference_bfs_ldbc_csv.py tools/ldbc_snb/test_statistics.py \
  tools/ldbc_snb/test_placement_report.py tools/test_polygraph_gui.py \
  tools/test_polygraph_instances.py
```

## Compatibility boundaries

Typed identities are additive C++ adapter fields, not new canonical JSONL syntax.
Legacy fingerprints, opaque keys, SQLite table layout and LDBC allocation remain
unchanged. Generated typed IDs depend on traversal order and placement; explicit
and generated payloads can collide and then fail. No skip/upsert policy, new
runtime parser, backend writer, public record-remapping/cache migration or
in-memory graph framework was introduced.
