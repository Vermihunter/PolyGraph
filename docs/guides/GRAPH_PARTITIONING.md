# Topology-aware native ingestion

KaMinPar is an optional shared-memory partitioner for the existing native ingestion pipeline. It runs before final ownership and ID assignment, then uses the same PostgreSQL COPY, Neo4j offline import and ArangoDB HTTP import executors as unpartitioned ingestion. Ordinary builds require neither KaMinPar nor TBB and reject `--partitioner kaminpar` explicitly.

The integration follows the graph projection and dense-index approach in the partition demo. The ingestion engine adds disk-backed source-key resolution, actual database imports, recovery and verification. Partition numbers remain separate from database identifiers and canonical GlobalId bits.

## Build

```bash
cmake --preset cpu-partition-tests
cmake --build --preset cpu-partition-tests --target \
  polygraph_bulk_import polygraph_bfs polygraph_ingestion_partition_test
build/cpu-partition-tests/bin/polygraph_ingestion_partition_test
```

CMake first looks for an installed KaMinPar package (3.7.3-compatible API). Otherwise, with `POLYGRAPH_FETCH_DEPENDENCIES=ON`, it fetches the pinned upstream revision `04dfa70cc3e6dbdf3c5fdf715d6e95beff267651` (v3.7.4 tag; its headers report 3.7.3). Only the shared-memory library is enabled. This requires oneTBB development files and first-configuration network access for missing dependencies, including KAssert. KAssert remains enabled for consistent upstream Debug initialization/assertions.

An existing source checkout can be reused **without modifying it**:

```bash
cmake --preset cpu-partition-tests \
  -DFETCHCONTENT_SOURCE_DIR_KAMINPAR=/absolute/path/to/KaMinPar
```

For a preinstalled package, set `KaMinPar_DIR` or `CMAKE_PREFIX_PATH`. With fetching disabled, missing dependencies fail configuration. The manifest records version/source provenance and SHA-256 of the actual loaded KaMinPar and TBB modules. Static KaMinPar conservatively fingerprints the executable, so rebuilding it requires a fresh partitioned output directory. This also catches dependency changes under an unchanged package version or dirty source revision. Installed-package and shared-library combinations beyond the tested pinned source build have not been live-tested.

To enable the same feature in the existing CLI location instead:

```bash
cmake --preset cpu-tests -DPOLYGRAPH_INGESTION_KAMINPAR=ON
cmake --build --preset cpu-tests --target polygraph_bulk_import
```

## Partition and import with one command

This preserves schema ownership and canonical IDs, adding partition properties:

```bash
build/cpu-partition-tests/bin/polygraph_bulk_import load \
  --adapter ldbc-snb --input data/ldbc-snb/sf0_1/raw \
  --placement-config tools/ldbc_snb/placement.schema-3db.json \
  --partitioner kaminpar --partitions 8 \
  --partition-threads 1 --partition-imbalance 0.03 \
  --partition-preset default --seed 42 \
  --threads 3 --memory-limit-mb 256 \
  --docker --keep-running \
  --output-dir build/ingestion-load/sf01-k8
```

The normal Docker isolation, fresh-target checks, offline Neo4j lifecycle, credentials, execution journals and cleanup commands apply. Strict artifact and live verification run automatically. Add `--verify-bfs --bfs-binary build/cpu-partition-tests/bin/polygraph_bfs` for the existing BFS runtime. `--skip-verification` and later `verify-load` remain available as described in [Docker ingestion](DOCKER_INGESTION.md).

To **place raw nodes by partition**, add this explicit mapping to a three-partition load:

```bash
build/cpu-partition-tests/bin/polygraph_bulk_import load \
  --adapter ldbc-snb --input data/ldbc-snb/sf0_1/raw \
  --placement-config tools/ldbc_snb/placement.schema-3db.json \
  --partitioner kaminpar --partitions 3 \
  --partition-map docs/examples/ingestion/kaminpar-backends.json \
  --docker --keep-running --output-dir build/ingestion-load/sf01-k3-mapped
```

Every generated partition must map to a configured storage backend; multiple partitions may share a backend. The mapping overrides **node** schema ownership before IDs are generated. As a result, mapped raw IDs may differ from schema-only IDs. `relationshipSources` still controls authoritative relationship ownership. Every edge is stored at the distinct union of its owner's and both endpoint owners' backends, with existing proxies where needed. Canonical JSONL IDs are always preserved; a map that contradicts their encoded ownership is rejected. No extra ghost replicas are introduced.

## Prepare without databases

```bash
build/cpu-partition-tests/bin/polygraph_bulk_import prepare \
  --adapter ldbc-snb --input data/ldbc-snb/sf0_1/raw \
  --placement-config tools/ldbc_snb/placement.schema-3db.json \
  --partitioner kaminpar --partitions 8 --seed 42 \
  --threads 3 --memory-limit-mb 256 \
  --output-dir build/ingestion-partition/prepared-sf01-k8

build/cpu-partition-tests/bin/polygraph_bulk_import verify \
  --manifest build/ingestion-partition/prepared-sf01-k8/manifest.json --strict
```

Both raw LDBC snapshots and `--adapter polygraph-jsonl` use the same partitioning stage. `prepare` never accesses databases. Inspect `manifest.json` → `partitioning` and its checksummed `assignment_artifact`. Assignment JSONL records have `key` and integer `partition`; raw keys are typed source IDs such as `Person:1`, while canonical keys are owner-only decimal IDs.

Existing `--partition-file` / `--partition-map` input remains supported without KaMinPar. It requires complete assignments and a backend map. External files and generated KaMinPar partitions are mutually exclusive. Unknown, duplicate, missing, fractional, negative or unmapped assignments fail. Changing source, placement, partition settings, map or dependency identity requires a fresh output directory. Resume checks both the SQLite index and assignment artifact; corrupt completed stages are rejected.

## Objective, determinism and memory

1. Stream all snapshot vertices into SQLite, sort source keys lexically, and assign temporary dense CSR coordinates. These coordinates are never persistent IDs.
2. Resolve relationship endpoints through SQLite. Convert directed arcs into unordered pairs; collapse reverse and parallel pairs; omit self loops from the objective. Isolated vertices remain present.
3. Build a symmetric CSR with unit vertex/edge weights and call real KaMinPar. Independently check returned assignments, balance and edge cut.
4. Persist assignments, stream original records through existing normalization and generate native artifacts. All original directed, parallel and self-loop relationships and their properties remain in the imported graph.

`--partitions` must be at least 2 and no greater than the nonempty vertex count. The projection uses 32-bit vertex coordinates; the actual KaMinPar build's edge/weight ranges are checked before conversion. Larger graphs may need a 64-bit KaMinPar build, but the ingestion vertex-coordinate limit remains 32-bit. Weighted objectives are unsupported. The presets are `default`, `eco`, and `strong`; default is `default`.

`--partition-imbalance` is the fractional KaMinPar allowance (default 0.03, range [0,1]). Unit vertex weights balance vertex counts, not bytes, degree, backend load or proxy counts. The manifest records actual block populations and the integer maximum block size from the library. Partition edge cut counts distinct undirected pairs once; `partition_cut_relationships` counts original directed relationships. `backend_cross_edges` measures endpoint ownership and is distinct from both. Keeping schema placement means a lower partition cut need not reduce database crossings.

`--partition-threads` controls KaMinPar separately from backend writer `--threads`. It defaults to 1. Lexical vertex order, sorted adjacency and a fixed seed give repeatable assignments for the tested sequential build and settings; parallel partitioning is not promised deterministic. Pin the dependency, executable, input, settings and thread count when comparing assignments. Calls within a process are serialized because the upstream RNG/state is global.

Properties and key lookups stay disk-backed. CSR and KaMinPar refinement require **O(V+E) RAM**. The preflight estimates input/copy/assignment buffers as `64*V + 32*E` bytes for undirected distinct edges and rejects an estimate above `--memory-limit-mb`. Refinement workspace, SQLite caches, record buffers and allocator overhead are additional: this flag is a budget, **not an RSS/cgroup limit**. Disk also holds the projection/index, assignments, native files and verification temporaries. Use SSD storage and allow substantially more space than the raw snapshot.

## Stored properties and verification

| Record | Generated properties |
|---|---|
| Authoritative node and existing proxy | `partition_id` (integer) |
| Authoritative edge and existing replica | `from_partition_id`, `to_partition_id` (integers), `is_partition_cut` (boolean) |

PostgreSQL stores these in existing `properties` JSONB; its tables and COPY columns are unchanged. Neo4j uses typed `:long`/`:boolean` CSV columns; ArangoDB uses JSON document properties. Input collisions with these generated fields are errors. Versioned metadata permits strict verification of older manifests without these fields. Basic verification checks assignment bytes/checksum; strict verification checks assignments against normalized endpoints/properties and reconstructs native artifacts. Live checks validate properties on every expected node/proxy and edge/replica in bounded batches.

The optional live regression covers raw and canonical snapshots, explicit backend mapping, exact IDs/endpoints/partition properties, corruption detection in each database and actual cross-backend BFS:

```bash
python3 tools/ingestion/docker_load_live.py --run \
  --binary build/cpu-partition-tests/bin/polygraph_bulk_import \
  --bfs build/cpu-tests/bin/polygraph_bfs \
  --partitioner kaminpar --cases keep canonical mapped \
  --output-dir build/ingestion-partition/live
```

With both `POLYGRAPH_INGESTION_LIVE_TESTS=ON` and KaMinPar enabled, CTest also registers `integration.ingestion.kaminpar`. Ordinary CPU tests remain service-free.

## Measure preparation overhead

```bash
python3 tools/ingestion/benchmark_partitioning.py \
  --binary build/cpu-partition-tests/bin/polygraph_bulk_import \
  --input data/ldbc-snb/sf0_1/raw \
  --placement-config tools/ldbc_snb/placement.schema-3db.json \
  --output-dir build/ingestion-partition/sf01-overhead \
  --partitions 2 8 --repetitions 2 --partition-threads 1 \
  --threads 3 --memory-limit-mb 256 --seed 42 --discard-artifacts
```

This compares `none`, k=2 and k=8 with the same executable/configuration, recording wall/CPU/RSS, final allocated disk, writer bytes/throughput, projection/library/assignment timings, cuts and populations. Strict verification and a streaming fingerprint of original IDs, properties, endpoints, placement and replica/proxy sets run **outside** preparation timing. Sequential assignment hashes must repeat. `--discard-artifacts` explicitly removes only each newly generated, verified preparation directory to save space; retained manifests, assignments, metrics and logs are evidence, not an executable import bundle. Omit it to retain complete artifacts. It never accesses Docker or databases.

See [measured SF0.1 partitioning overhead](../benchmarks/INGESTION_PARTITIONING.md). No native-load or traversal speedup is inferred from a preparation benchmark. Larger scale factors, weighted objectives, automatic cut-driven ghost replication and runtime partition scheduling remain unsupported or unmeasured.
