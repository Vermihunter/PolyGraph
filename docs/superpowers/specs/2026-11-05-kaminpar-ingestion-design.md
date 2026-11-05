# Optional KaMinPar ingestion

Implement the user's partition-then-load workflow in the existing standalone ingestion engine. Reference: `/home/akos/Coding/kaminpar-partition-demo` (CSR conversion, KaMinPar v3.7.4 library, original-ID mapping and logical ghost suggestions). Reuse the existing adapters, SQLite spool, ID codec, placement/proxy rules, native writers/executors and isolated Docker lifecycle. Do not modify the demo or existing databases.

## Approach

Use the actual shared-memory C++ library behind the existing OFF-by-default `POLYGRAPH_INGESTION_KAMINPAR` switch. Prefer an installed package; optionally fetch the pinned v3.7.4 commit `04dfa70cc3e6dbdf3c5fdf715d6e95beff267651`. CLI subprocess partitioning would add an unnecessary text graph and runner protocol; copying the demo's in-memory readers would duplicate PolyGraph validation and retain properties in RAM. Instead, stream the existing adapter into disk-backed source-key/dense-index and deduplicated topology tables, then construct bounded, size-checked CSR buffers.

The projection contains all snapshot nodes, symmetrizes directed relationships, collapses parallel/reverse arcs to one unit-weight undirected edge and excludes self loops from the partition objective. Original directed/multiple/self-loop records remain unchanged for import. Balance unit vertex counts with configurable count, imbalance, seed, threads and library preset. Default partition threads: 1; fixed input/seed/settings are reproducible in the tested sequential mode. Parallel KaMinPar can be nondeterministic and is reported accordingly. Serialize library calls because its seed/timers are process-global. Document O(V+E) CSR and additional library workspace; the existing preparation budget is not a hard RSS cap. Fail before allocation when required buffers exceed the budget. Never silently substitute another algorithm.

## Placement and identity

Partition numbers are logical blocks, never backend bits. Without a partition map, schema node/relationship ownership and canonical IDs remain unchanged. With an explicit map, assign raw node ownership before finalizing its existing block31 payload. Canonical JSONL IDs must be preserved; incompatible remapping fails. Relationship ownership continues to follow `relationshipSources`. Endpoint resolution, edge replicas and one proxy per required backend/remote endpoint use the existing rules. Logical ghosts do not create extra physical vertices or authoritative owners.

## Persisted metadata

For partitioned preparation, add integer `partition_id` to native nodes and proxies. Add integer `from_partition_id`, `to_partition_id` and boolean `is_partition_cut` to relationships and replicas. Reject colliding input property names rather than overwriting user data. Neo4j uses typed CSV property columns; ArangoDB uses JSON properties; PostgreSQL uses its existing JSONB properties and unchanged COPY/table schema. Store source-key assignments in the existing SQLite `partitions` table and publish a checksummed reusable assignment JSONL artifact. Measure simple undirected cut separately from original relationship cuts and backend crossings; report partition populations/balance and per-relation cut statistics.

Unpartitioned output remains compatible. Version the additive partition metadata in configuration, include all relevant settings/dependency identity in recovery hashes, and preserve strict verification of older manifests. Strict verification checks assignment artifacts/closure/metadata, regenerates native representations and detects corruption. Live verification must validate partition fields as well as existing IDs/counts/endpoints. Do not change the BFS/query runtime.

## CLI and validation

Expose `--partitioner none|kaminpar`, `--partitions N`, `--partition-threads N`, `--partition-imbalance E`, `--partition-preset NAME`, existing `--seed`, `--partition-map` and `--partition-file` through prepare and Docker load. KaMinPar requires an explicit count (2..vertex count); generated assignments and an external assignment file are mutually exclusive. External files retain their explicit mapping behavior. Default prepare remains offline and ordinary CPU tests require no KaMinPar/Docker.

Test real KaMinPar partitions, deterministic sequential runs, sparse/signed canonical IDs, malformed graphs/options/maps, source-key collisions, endpoint failure, projection deduplication/loops/isolates, metadata collisions, all backend representations, proxies, unchanged unpartitioned IDs and stale/corrupt artifacts. Run dedicated fresh Docker ingestion, exact metadata verification and cross-backend BFS. Benchmark preparation with none versus real partitioning on SF0.1, separating projection/library/metadata work, CPU/RSS/disk and cut/balance. Match executable/input/settings, verify artifacts outside timing and publish actual measurements with limitations. Larger-scale or traversal-speed benefits are not assumed.
