# Independent CSV correctness oracle

This package is test-only and uses Python 3.10 or newer's standard library. **Production graph
loaders, ID conversion, routing, placement, query code, and their helpers must
not be imported or reused.** `test_independence.py` checks imports and executes a
fresh interpreter with production imports and production-file reads blocked.

The schema facts are authored in `schema.py` from the
[SNB specification](https://ldbcouncil.org/ldbc_snb_docs/) and the
[Interactive v1 dataset format](https://ldbcouncil.org/benchmarks/snb/datasets/).
This oracle uses the Hadoop/CsvBasic initial snapshot, not Interactive v2 or BI.

## Graph and BFS contract

- A vertex is `(entity_type, original unsigned 64-bit LDBC ID)`. The eight CSV
  entity types remain distinct even when their numeric IDs coincide. There are
  no backend, routed, native-database, or cache-decorated IDs here.
- All 23 relation families are loaded with their typed source/target endpoints.
  Duplicate relationship rows are retained. Projections use unique neighbors.
- Only the initial `static/` and `dynamic/` topology is loaded. Email/language
  files are validated as Person attributes; they do not add graph edges.
  Root update streams and Hadoop CRC files are excluded.
- The default `both` projection traverses every relationship in either direction.
  `outgoing` follows CSV source-to-target orientation, including the one stored
  orientation of knows. It is a stored-orientation check, not a claim that knows
  is logically directed. Edge filters apply before adjacency union. Target-type
  filters apply to each transition, while the root is always retained.
- FIFO BFS discovers on enqueue. Sorted typed neighbors make results and parent
  choices deterministic. Depth zero returns only the source; depth K includes
  distance K but does not expand that boundary; null means unlimited. Missing
  roots and negative/noninteger depths are rejected.

`test_bfs.py` uses manually checked chains, diamonds, cycles, self-loops,
duplicates, disconnected/isolated vertices and filters. It also checks 3,072
small generated cases against a separate Floyd–Warshall distance matrix.

## Strict comparison

Accepted result CSV schemas are exactly:

```text
vertex_type,vertex_id,distance
vertex_type,vertex_id,distance,predecessor_type,predecessor_id
```

IDs and distances use canonical decimal text, without signs, whitespace, floats
or leading zeros. Parents are either a complete typed pair or two empty fields.
Duplicate vertices (even identical repeated rows), malformed rows/headers,
unknown types and ID overflow are errors.

Comparison requires exact vertex/distance maps. With parents enabled, every
reached non-root needs one allowed parent edge from distance d-1; the root must
have none. Any valid shortest tree is accepted. With parents disabled the parent
map must be empty. Ordering, counts alone, and histogram equality do not establish
correctness. The CLI recomputes expected results from CSV rather than trusting an
external expected-result file. It does not decode PolyGraph result IDs.

## Pinned local SF0.1 snapshot and roots

`ldbc_sf01.lock.json` records SHA256, byte counts, CSV headers and logical row
counts for all 33 CSV shards and `params.ini`. It also records the independent
schema digest and entity/relationship counts. Unknown or changed shards, schema,
counts, parameters and lock metadata fail verification. Verification never
rewrites the lock.

This is an enrollment of the **local** SF0.1 snapshot: 327,588 vertices and
1,477,965 relationship rows. Generator commit, seed and original archive checksum
are explicitly unknown, not claimed as verified upstream provenance. `params.ini`
declares SF0.1, CsvBasic serializers and LongDateFormatter. Pinning these bytes does
not certify an official benchmark run.

`ldbc_sf01_queries.json` binds to the complete lock digest and contains 24 explicit
typed roots and 41 queries. Roots include per-type minimum IDs and maximum
distinct bidirectional degree (ties by smallest ID), then SHA256-ranked vertices
to reach 24. If a minimum and maximum coincide, the root appears only once.
Queries cover Both depth 7, depth boundaries including zero/unlimited, outgoing,
knows-only, target-type filtering, and distance-only output. Neither roots nor
expectations come from a database, PolyGraph converter, or placement policy.

## Commands

Run the hand-checked, corruption, manifest, CLI and independence tests without
the real dataset, databases, Docker, third-party Python packages or a GPU:

```bash
python3 -m unittest discover -s tests/reference -t . -p 'test_*.py' -v
ctest --preset cpu-tests -R python.reference_oracle --output-on-failure
```

Verify and explicitly run against the local raw snapshot:

```bash
python3 -m tests.reference verify \
  --dataset data/ldbc-snb/sf0_1/raw --lock tests/reference/ldbc_sf01.lock.json
python3 -m tests.reference run \
  --dataset data/ldbc-snb/sf0_1/raw --lock tests/reference/ldbc_sf01.lock.json \
  --queries tests/reference/ldbc_sf01_queries.json --output-dir build/reference-sf01
```

Choose a new/empty output directory on each run. Each query gets a canonical CSV,
result checksum, exact layer histogram and semantic parent/round-trip validation.
The first result is deliberately corrupted five ways, checking rejection of a
missing vertex, extra vertex, wrong distance, invalid parent and duplicate row.
Corrupt CSV artifacts live separately under `corruptions/`. `report.json` is
written only after every query and corruption check succeeds.

Compare a canonical raw-ID result for a fixed query:

```bash
python3 -m tests.reference compare \
  --dataset data/ldbc-snb/sf0_1/raw --lock tests/reference/ldbc_sf01.lock.json \
  --queries tests/reference/ldbc_sf01_queries.json --query person-min-both-d7 \
  --actual build/reference-sf01/person-min-both-d7.csv --report build/comparison.json
```

Exit status is 0 for success, 1 for a semantic mismatch, 2 for malformed input or
configuration. Enrollment commands `lock` and `roots` require a new output file;
they must not be used to accept drift in an existing pinned corpus casually.

Raw SF directories and `build/` are already ignored by the repository's
`.gitignore`. Track the independent package, lock and explicit queries, not the
large raw data or generated reference outputs. No dataset is auto-downloaded or
auto-run by CMake/CTest. This stage validates the oracle, not database adapters or
PolyGraph execution results.
