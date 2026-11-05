# Native ingestion partitioning overhead

This experiment measures optional real KaMinPar preparation on the existing LDBC SNB SF0.1 snapshot. It compares no partitioning with k=2 and k=8, retaining schema placement in all cases. The source contains **327,588 nodes and 1,477,965 original directed relationships**. Full machine-readable trials, input hashes, executable/source/dependency identities and resource measurements are in [the measurement record](data/native-ingestion-partitioning-sf0_1-2026-11-05.json).

## Reproduce

```bash
cmake --preset cpu-partition-tests
cmake --build --preset cpu-partition-tests --target polygraph_bulk_import
python3 tools/ingestion/benchmark_partitioning.py \
  --binary build/cpu-partition-tests/bin/polygraph_bulk_import \
  --input data/ldbc-snb/sf0_1/raw \
  --placement-config tools/ldbc_snb/placement.schema-3db.json \
  --output-dir build/ingestion-partition/sf01-overhead \
  --partitions 2 8 --repetitions 1 --partition-threads 1 \
  --threads 3 --memory-limit-mb 256 --seed 42
```

The tested build reused the demo's read-only cached upstream source with `-DFETCHCONTENT_SOURCE_DIR_KAMINPAR=/home/akos/Coding/kaminpar-partition-demo/build/_deps/kaminpar-src`. It used the pinned `04dfa70cc3e6dbdf3c5fdf715d6e95beff267651` revision (v3.7.4 tag/header 3.7.3), KAssert enabled, Debug GCC 11.4, default preset and 3% allowed vertex imbalance. No concurrent builds, tests or new Docker imports ran during measurement. Other pre-existing services and development processes remained active.

GNU time measures complete native `prepare` wall/user/system time and maximum process RSS. It includes hashing, projection, partitioning, normalization, all backend writers, fsync and publication. After **each** timed preparation, the helper runs strict artifact reconstruction and streams a fingerprint of original IDs, residency, properties, endpoints, schema placement, proxies and replicas. These correctness checks are outside preparation timing. The helper checks sequential fixed-seed assignment hashes whenever repetitions are requested. This published run uses one repetition; repeatability is covered separately by small-graph tests. Preparation uses no live databases; these results do not measure native loading or BFS throughput.

`--discard-artifacts` optionally saves each verified manifest, measurement, assignment JSONL and logs, then removes only the fresh trial's complete artifact directory. Such reduced trial directories cannot be executed as import bundles. The published rerun omitted that flag and retained complete bundles. The public JSON retains the full trial metrics with dataset paths made repository-relative; complete private evidence is retained under `/tmp/polygraph-kaminpar-sf01-20261105-rerun-001` and `build/ingestion-partition/sf01-overhead-complete-001`.

The initial two-repetition campaign was invalidated when its completed output directories disappeared during the final preparation. That run failed; its partial figures are excluded. The published rerun uses a fresh temporary directory on the same ext4 NVMe filesystem and one complete sample per variant. It provides no estimate of variance.

## Measured results

Host: AMD Ryzen 7 5800H, 16 logical CPUs, 31.2 GiB RAM, Linux 6.8, `powersave` governor. Swap was almost full and approximately 4.7 GiB RAM was available at the start. These conditions limit generalization.

| Preparation mode | Wall seconds | Added time | CPU seconds (user + system) | Average CPU | Peak RSS MiB | Allocated artifacts/index MiB |
|---|---:|---:|---:|---:|---:|---:|
| none | 84.61 | — | 109.45 | 129.4% | 228.62 | 788.89 |
| KaMinPar k=2 | 143.50 | +58.89 s / +69.6% | 193.59 | 134.9% | 273.81 | 1044.09 |
| KaMinPar k=8 | 146.97 | +62.36 s / +73.7% | 197.00 | 134.0% | 283.66 | 1043.98 |

Complete preparation throughput, using input counts divided by wall time:

| Mode | Nodes/s | Original relationships/s |
|---|---:|---:|
| none | 3,871.74 | 17,467.97 |
| k=2 | 2,282.84 | 10,299.41 |
| k=8 | 2,228.94 | 10,056.24 |

| Partition phase (seconds) | k=2 | k=8 |
|---|---:|---:|
| SQLite projection and CSR construction | 16.32 | 16.62 |
| KaMinPar compute only | 4.55 | 6.82 |
| Whole library wrapper, including compute/copy/checks | 6.18 | 8.48 |
| Assignment persistence | 0.38 | 0.37 |
| Metadata/cut/export work | 5.64 | 6.19 |
| Complete partition stage | 28.64 | 31.79 |

Compute time is included in the wrapper time; do not add both. The remaining full-preparation overhead includes partition lookups, larger normalized properties and larger native artifacts. KaMinPar compute itself accounts for only part of the measured added time. Both runs used a 14,432,984-byte CSR and a conservative 68,214,720-byte input/copy buffer estimate, with additional workspace/cache memory reflected in RSS.

| Partition metric | k=2 | k=8 |
|---|---:|---:|
| Distinct undirected cut edges | 235,394 | 514,056 |
| Original cut relationships | 235,783 | 514,789 |
| Maximum allowed block size | 168,707 | 42,177 |
| Actual minimum/maximum block size | 158,881 / 168,707 | 40,206 / 42,177 |

Both assignments satisfy the library balance bound. Full populations, relation-specific cuts, assignment checksums, per-backend counts and timings are retained in JSON. Larger k makes more potential boundaries; comparing raw cuts across k is not a partition-quality ranking.

Backend **artifact writer** throughput (decimal MB/s; no database loading):

| Writer | none | k=2 | k=8 |
|---|---:|---:|---:|
| ArangoDB | 8.00 | 6.99 | 6.95 |
| Neo4j | 7.18 | 3.41 | 3.42 |
| PostgreSQL | 10.14 | 11.36 | 11.37 |

These writers emit different amounts of data when partition properties are enabled. Their bytes/s cannot be interpreted as equal-work ingestion speedups. Verification plus fingerprinting took 81.24 s, 162.13 s and 162.55 s respectively, outside the preparation timer. All strict checks passed and all three fingerprints match. Sequential SF0.1 repeatability was not measured in this completed single-sample campaign; small-graph tests cover fixed-seed repeatability.

## Interpretation and limits

The objective is a simple undirected unit-weight graph: **1,476,534 distinct pairs**, collapsing 1,431 reverse/parallel records; no self loops occur in this snapshot. The original records are all retained for import. CSR coordinates never become canonical IDs. Schema placement stays unchanged, so all variants retain **923,931 cross-backend endpoint relationships**; partition cut is a separate metric. A partition label alone does not move data or improve distributed traversal. An explicit partition-to-backend map can move raw node ownership, but its loading/traversal performance was not measured here.

Partitioning adds projection work, refinement, assignment persistence/validation and extra native properties. Database artifact/index size grows even when placement stays the same. Compare full preparation overhead with the separately recorded library compute time; a low KaMinPar time does not include the rest of this pipeline.

This is one sample per variant on one shared host with uncontrolled caches/swap and fixed trial order (`none`, k=2, k=8). They establish a reproducible development baseline, not statistical evidence across machines or scale factors. The memory flag is a buffer/cache budget, not an RSS bound. Final allocated artifact disk is measured before discard, not peak temporary disk or I/O volume. Strict verification also uses temporary disk and scanning time. Weighted objectives, larger scales, parallel partition determinism, installed/shared-library deployment combinations and native-load/traversal benefits remain unmeasured.

## Correctness and regression evidence

The final reviewed implementation passed:

- Ordinary CPU CTest: **519/519** tests, without KaMinPar or database services.
- Enabled ingestion CTest selection: **50/50**, including **11** actual KaMinPar/projection tests and **36** ingestion tests.
- Python ingestion modules: **38/38**; graph I/O discovery: **31/31**; explicit loader/LDBC modules: **16/16** (12 LDBC and 4 loader tests).
- Old unpartitioned and external-assignment manifests generated by the earlier executable passed the new strict verifier.
- Focused partition API ASan/UBSan with leak detection: four cases repeated 100 times. The identical command needed to run outside the ptrace sandbox for LeakSanitizer thread inspection. The upstream library was not sanitizer-instrumented.
- Actual isolated PostgreSQL 17, Neo4j 5.26 Enterprise and ArangoDB 3.12 imports: raw schema placement, canonical JSONL and raw explicit partition mapping; exact live IDs/counts/endpoints/partition properties and cross-backend CPU BFS. All three cases passed. Six deliberate node/edge partition-property corruptions (two per backend) failed verification, were restored, and then passed without rewriting the import journal. All 48 pre-existing containers and all 33 unrelated container states were preserved.

Exact verification commands:

```bash
ctest --preset cpu-tests --output-on-failure
ctest --preset cpu-partition-tests \
  -R 'IngestionPartition|IngestionTest|python.ingestion' --output-on-failure
POLYGRAPH_BULK_IMPORT=build/cpu-partition-tests/bin/polygraph_bulk_import \
  python3 -m unittest tools.ingestion.test_semantics \
  tools.ingestion.test_docker_workflow tools.ingestion.test_benchmark_ldbc \
  tools.ingestion.test_partition_benchmark -v
python3 -m unittest discover -s tools/graphio -p 'test_*.py' -v
python3 -m unittest tools.graphio.test_loaders \
  tests.test_ldbc_snb_conversion tests.test_ldbc_fast_prepare -v
python3 tools/ingestion/docker_load_live.py --run \
  --binary build/cpu-partition-tests/bin/polygraph_bulk_import \
  --bfs build/cpu-tests/bin/polygraph_bfs --partitioner kaminpar \
  --cases keep canonical mapped --output-dir build/ingestion-partition/reviewed-live
```

The final live report is `build/ingestion-partition/reviewed-live/run-45e009acfc51/result.json`. Owned containers/networks were removed afterward. Logs, artifacts and private lifecycle evidence remain in ignored output. The source snapshot, demo, runtime/BFS code and existing databases were unchanged. See [the usage guide](../guides/GRAPH_PARTITIONING.md) for build options, mappings, metadata and recovery.
