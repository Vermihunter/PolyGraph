# Runtime source identity implementation plan

**Goal:** Implement #34 under Phase 3 #26: resolve generic typed source roots
using normalized ingestion metadata without changing distributed BFS or queries.

**Architecture:** Runtime schema/type filters/backend adapters already use logical
metadata. Extend the BFS CLI/domain defaults with an explicit typed source root,
resolved by a bounded read-only SQLite lookup in the existing prepared index.
Keep canonical UUID and native-ID-map root behavior. Share the v1 source identity
value/encoding between ingestion and runtime; do not introduce another pipeline.

**Spec:** GitHub #34, #26 and logical-graph-contract.md; #32 source identities and
#33 native workflow are completed dependencies. No AGENTS.md found in source or
ancestor directories during repository inspection.

## Decisions

- `--start-source` is a JSON object with namespace, logical type, explicit
  idType (string/int64/uint64) and exact id value. Domain default is
  `defaults.bfs.startSource`; startUuid stays supported. Explicit CLI roots
  override domain defaults, and specifying both CLI selectors is an error.
- Reuse `paths.ingestionArtifacts/manifest.json` to locate the prepared SQLite
  index, with `--source-index <manifest.json>` as an explicit compatibility path.
  Only source-root lookup reads it; canonical roots do not gain a new dependency
  on artifact existence. No copying a full identity map into RAM.
- Validate prepared version/state/typed strategy, confined index path, byte size
  and SQLite lookup. Missing/type/namespace mismatches fail without numeric or
  property fallback. Routing must match the stored owner. Full cryptographic
  artifact verification remains the existing import/verify command's job.
- Preserve shared source-key bytes and ingestion Error behavior when extracting
  the generic representation. Runtime links SQLite, not the ingestion engine.
- Schema/type filters and physical queries remain generic and stay unchanged.
  Test actual BFS with resolved arbitrary source IDs and logical type filters
  using existing one-hop test stores; integration lookup uses real preparation.
- Alternatives: scanning arbitrary source properties would be ambiguous; loading
  all source mappings in memory violates streaming constraints; adding a second
  exported map format duplicates the existing SQLite index.

## Tasks

- [x] Add failing runtime/CLI tests for exact typed IDs, schema types, domain root
  precedence, missing IDs and canonical/LDBC compatibility.
- [x] Share source identity encoding and add read-only prepared-index lookup;
  wire additive domain/CLI selectors with context routing validation.
- [x] Exercise native preparation -> typed lookup -> real BFS executor with
  multiple logical types, generic edge filters, native remapping and failures.
- [x] Document runtime input trace and source-root usage/limits.
- [x] Run CPU, relevant KaMinPar/Python, ingestion-disabled runtime build, docs,
  links/format and independent review. Report live/GPU checks not executed.

No automatic commit/closure for #34 or starting #35.

## Verification record

- Initial red build failed on missing `cli/SourceNodeIdResolver.hpp`; runtime
  selectors/resolver were absent. Focused integration subsequently exposed an
  incomplete test context (no registered owner store), corrected in the fixture.
  A routing test was corrected to remove the owner: location masks are fixed bits,
  so reordering configured owners does not change an ID's owner.
- `cmake --build --preset cpu-tests -j 2`: passed.
- `ctest --test-dir build/cpu-tests --output-on-failure -j 2`: 574 passed,
  2 optional KaMinPar skips, zero failures (576 registered tests). Includes 13
  new runtime/encoding tests and existing BFS/ingestion/concurrency regressions.
- `cmake --build --preset cpu-partition-tests --target
  polygraph_runtime_source_test polygraph_source_identity_test
  polygraph_input_adapter_registry_test polygraph_ingestion_partition_test
  polygraph_bulk_import -j 2`: passed.
- `ctest --test-dir build/cpu-partition-tests -R
  '^unit.RuntimeSource|^unit.SourceIdentity|^unit.SourcePipeline|^unit.InputAdapter|^unit.IngestionPartition|^python.import_workflow|^python.graph_manifest'
  --output-on-failure`: 55 passed, zero failures/skips. Includes actual KaMinPar
  and both CPU-skipped adapter replay paths.
- Python regression selection below: 219 passed, 3268 subtests passed.
- A separate Release BFS configuration with `POLYGRAPH_BUILD_BULK_IMPORT=OFF`
  built successfully in `build/issue34-runtime-only`. Cached spdlog/argparse
  **sources** were supplied via separate FETCHCONTENT_SOURCE_DIR values; build
  directories were not shared. That binary ran the checked-in generic domain
  at depth zero, producing `281474976710657;0` without database requests.
- The actual BFS executable also resolved a prepared typed-source domain default
  at depth zero. Real BFS executor integration used existing scripted one-hop
  stores for generic logical filters/direction and LDBC cross-backend distances
  (Person 0, Post 1, Place 2); no fake BFS algorithm was introduced.
- `cmake --build --preset docs`, `python3 tools/docs/check_links.py`,
  clang-format dry-run on changed C++ and `git diff --check`: passed.
- Independent review found no substantive issues. Its optional typed-root plus
  native-ID-map coverage suggestion was added using the actual native resolver.
- Existing Doxygen unsupported-option, deprecated SQLite CMake target alias and
  Conda libcurl version-information warnings remain. Live database/Docker
  traversals, HTTP application build and GPU execution were not run.

```sh
POLYGRAPH_BULK_IMPORT="$PWD/build/cpu-tests/bin/polygraph_bulk_import" \
  python3 -m pytest -q tools/graphio tools/ingestion tests/reference \
  tests/test_ldbc_snb_conversion.py tests/test_ldbc_fast_prepare.py \
  tests/test_reference_bfs_ldbc_csv.py tools/ldbc_snb/test_statistics.py \
  tools/ldbc_snb/test_placement_report.py tools/test_polygraph_gui.py \
  tools/test_polygraph_instances.py

cmake --preset cpu-bfs -B build/issue34-runtime-only \
  -DPOLYGRAPH_BUILD_BULK_IMPORT=OFF \
  -DPOLYGRAPH_ENABLE_CLANG_FORMAT=OFF -DPOLYGRAPH_ENABLE_CLANG_TIDY=OFF \
  -DFETCHCONTENT_SOURCE_DIR_SPDLOG="$PWD/build/cpu-tests/_deps/spdlog-src" \
  -DFETCHCONTENT_SOURCE_DIR_ARGPARSE="$PWD/build/cpu-tests/_deps/argparse-src"
cmake --build build/issue34-runtime-only --target polygraph_bfs -j 2
build/issue34-runtime-only/bin/polygraph_bfs \
  --domain docs/examples/ingestion/logical-graph/domain.json \
  --max-depth 0 --thread-count 1 --no-logging --no-events \
  --output-file build/issue34-runtime-only/canonical-root.csv
```

## Acceptance and compatibility review

Generic schema/runtime construction, filters and source-root BFS work without
LDBC labels. The checked-in raw LDBC fixture passes preparation, legacy root
selection and hand-checked cross-backend BFS distances. Signed/unsigned/string
source values, multiple logical types, native remapping, missing namespaces,
invalid selectors, malformed/confined index paths and read-only lookup are covered.

Runtime now requires SQLite development libraries, with the importer disabled or
enabled; it never links the ingestion engine. Shared identity v1 bytes and the
source-compatible ingestion alias/helper are preserved, but static clients need
rebuilding. Source roots are an additive BFS CLI/domain feature; Python benchmark
wrappers and the HTTP public root format continue using GlobalIds. Runtime lookup
does not hash the whole index or validate live graph freshness; immutable artifacts
from the matching graph generation and existing verification remain required.
