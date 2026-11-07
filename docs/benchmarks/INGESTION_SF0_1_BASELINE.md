# LDBC SNB SF0.1 native-ingestion baseline

One measured point, collected on **2026-11-04**, for raw CsvBasic snapshot ingestion into fresh Neo4j, ArangoDB and PostgreSQL databases. [Machine-readable data](data/native-ingestion-sf0_1-2026-11-04.json) includes input-file hashes, image IDs, settings and resource counters. This is an initial single-scale baseline; increasing-scale evaluation has not been performed.

The snapshot contains **327,588 nodes and 1,477,965 relationships** in 61.62 MiB of snapshot CSV files. Root update streams are excluded. The existing [three-backend placement](../../tools/ldbc_snb/placement.schema-3db.json) produces **3,342,670 physical node/proxy/relationship records** after replication. There is no partitioning. Exhaustive correctness verification is deferred with `--skip-verification`; normal parsing, endpoint resolution, checksums, fresh-target safeguards and importer acknowledgements remain enabled.

Host: AMD Ryzen 7 5800H, 16 logical CPUs, 31.20 GiB RAM, `powersave` governor, Linux 6.8.0-138. Engine revision: `0ab26cd3a53624c00f25ab441e8dba63001aa2d2`; **Debug** build using GCC 11.4 and the `cpu-tests` preset. Three preparation threads, 256 MiB client cache/record budget, default three-backend execution concurrency. Docker 29.1.3; cached Neo4j 5.26 Enterprise, ArangoDB 3.12 and PostgreSQL 17 images are pinned by ID in the data file. Host load, swap state and OS/data caches were not controlled.

## Timings and throughput

| Phase | Elapsed | Throughput |
|---|---:|---:|
| Preparation, internal timer | 86.440 s | 20,888 logical entities/s |
| Concurrent native backend execution | 224.107 s | 14,915 physical records/s |
| End-to-end Docker load, GNU time | **317.460 s (5m17.46s)** | **5,687 logical entities/s** |

Preparation includes normalization, SQLite resolution and artifact generation, but its internal timer stops before final backend checksum validation and manifest publication. The native-execution timer starts after initial integrity checks and global preflight, and includes Neo4j startup plus index/repair work. The end-to-end timer includes preparation, provisioning and loading, but excludes correctness verification, subsequent resource-report collection and cleanup. These timers have different boundaries; their sum is not the complete elapsed time. Logical entities means nodes plus relationships; physical records also include proxies and replicated relationships.

| Backend | Reported native-import time | Physical records | Records/s |
|---|---:|---:|---:|
| PostgreSQL | 1.875 s | 632,191 | 337,155 |
| Neo4j | 15.686 s | 1,080,859 | 68,904 |
| ArangoDB | 224.104 s | 1,629,620 | 7,272 |

Backend times overlap. PostgreSQL's timer includes COPY/ANALYZE/commit; ArangoDB's includes collection creation, HTTP import and UUID indexes. Neo4j's timer measures **only the offline administrative importer**; subsequent startup, indexes and property repairs are included in concurrent execution time. Backend rates therefore describe different work and are not directly comparable.

## Resources

- Local load process family: **111.24 CPU seconds user + 4.87 seconds system**, averaging 36.57% of one CPU over the complete load; **223.83 MiB maximum process RSS**. CPU excludes Docker daemon/container processes; RSS is not the sum of concurrent processes.
- Docker peak sample-round memory sum: **2.523 GiB**, sampled every two seconds. Each round sums sequential cgroup-memory readings, including page cache/kernel usage. This is not an instantaneous total-RAM peak, and it must not be added to client RSS as an exact whole-host peak.
- Final allocated artifacts plus SQLite index: **788.94 MiB**. Database bind directories: **1.558 GiB total**, measured with `du -sk` while services were running. These are final allocated blocks, not peak temporary space or disk I/O volume.

| Container role | CPU seconds through last sample | Sampled peak cgroup memory |
|---|---:|---:|
| PostgreSQL | 3.24 | 253.26 MiB |
| ArangoDB | 237.07 | 1,256.42 MiB |
| Neo4j offline importer | ≥57.07 | 1,174.00 MiB |
| Neo4j server | 52.85 | 1,297.34 MiB |

The auto-removed offline importer's final CPU tail may be missed. Between-sample memory spikes may also be missed. Resource samples and phase timers are not synchronized per phase; the CPU figures cover each observed container lifetime through its last sample, rather than isolated importer CPU time.

## Bottleneck and interpretation

**ArangoDB determined the native-loading critical path:** its 224.104 s closely matches the 224.107 s concurrent execution phase. The current implementation sends HTTP import batches serially and creates UUID indexes afterward. Profiling those operations is the first optimization target; this single observation does not separate API parsing, index construction and storage costs.

Preparation used 86.440 s; normalization alone processed approximately 27,160 nodes/s and 42,766 relationships/s. SQLite resolution, replication, serialization and checksums add work beyond parsing. This baseline measures import-only loading. Moving verification into a separate command changes the measured workload; it is not evidence of an equivalent-work speedup over fully verified ingestion or the Python loader.

## Reproduce or extend

Requires local Linux Docker access through a Unix socket, GNU `/usr/bin/time`, `du`, the native CLI and Python 3.10+ (standard library only).

```bash
cmake --preset cpu-tests -DPOLYGRAPH_INGESTION_LIVE_TESTS=OFF
cmake --build --preset cpu-tests --target polygraph_bulk_import

python3 tools/ingestion/benchmark_ldbc.py \
  --binary build/cpu-tests/bin/polygraph_bulk_import \
  --input data/ldbc-snb/sf0_1/raw \
  --placement-config tools/ldbc_snb/placement.schema-3db.json \
  --scale-factor 0.1 --docker --threads 3 --memory-limit-mb 256 \
  --sample-interval-seconds 2 \
  --output-dir build/ingestion-benchmark/ldbc-sf0_1-new
```

Use an absent output directory. The collector reuses `load --docker --keep-running --skip-verification`, measures only its generated ownership-labelled containers, then removes those containers/network after measurement. Artifacts, private credentials, database bind files, GNU time output and Docker samples remain under ignored `build/` output. It never resets existing services. Cancellation waits for the existing load controller's graceful cleanup; an unresponsive Docker daemon has no forced shutdown deadline.

The measured run is retained at `build/ingestion-benchmark/ldbc-sf0_1-20261104-001/`. Its original collector snapshot is `collector-at-launch.py`, matching the hash in the data. The current helper additionally protects untimed final cleanup against interruption and captures provenance before launch; timed loading and metric aggregation are unchanged.

For larger scale factors, change `--input`, `--scale-factor` and the fresh output directory, keeping build, placement, images and settings fixed. Repeat trials before drawing scaling or variance conclusions. **Only SF0.1 and one measured repetition are published here; SF1/SF10, statistical variation and native-versus-Python performance comparisons remain unmeasured.**
