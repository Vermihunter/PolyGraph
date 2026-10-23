# SF1 distribution matrix

This experiment compares logical placement independently from cache policy. It keeps the LDBC SNB SF1 logical graph and BFS workload fixed while changing the persistent owner of nodes and relationships across Neo4J, ArangoDB and PostgreSQL.

## Distributions

The matrix contains the existing `schema-based` placement plus four additional distributions.

### `hash-attributes`

Records are routed by a stable hash of configured attributes. Node routing uses `($type, ldbc_id)` by default. Relationship routing uses `($type, $from, $to)` and selected relationship properties such as `creationDate`, `joinDate`, `classYear` or `workFrom` where they exist.

The hash is deterministic and maps into the ordered backend list `[Neo4J, ArangoDB, PostgreSQL]`. The same raw input therefore produces the same placement on every run.

### `label-striped-3`

This is the 1/N placement for N=3. Every logical label is independently striped over all three databases by a deterministic per-record hash. For example, `Person` records are not assigned as one schema unit: approximately one third of Persons go to Neo4J, one third to ArangoDB and one third to PostgreSQL. The same rule is applied independently to every node and relationship label.

The split is deterministic and statistically 1/3 rather than an exact cardinality partition. This keeps conversion streaming and lets relationship endpoint ownership be recomputed from `(type, id)` without retaining an O(|V|) owner table. The generated placement report shows the realized fractions.

### `schema-family`

This is a second schema-driven placement, but its grouping logic differs from the existing schema-based configuration:

- Neo4J: actor/organization entities (`Person`, `Organisation`) and relationships originating from those semantic families.
- ArangoDB: conversation/content entities (`Forum`, `Post`, `Comment`) and relationships originating from those families.
- PostgreSQL: reference/taxonomy entities (`Place`, `Tag`, `TagClass`) and relationships originating from those families.

The intent is to keep interaction-heavy social/content paths together while separating relatively static reference/taxonomy data. Relationships are assigned according to their schema-defined source family rather than hand-balancing individual relationship types.

### `proxy-minimized`

This placement is generated from the actual SF1 dataset by `tools/ldbc_snb/optimize_proxy_placement.py`.

For the eight LDBC node types it evaluates all `3^8 = 6561` whole-type assignments. Under the configured balance constraints it minimizes the exact number of unique physical proxy copies implied by the loader's endpoint-replication semantics. For each authoritative node the objective counts the number of *distinct remote backend owners* among its neighbors; that is exactly the number of other endpoint backends on which that node needs a Proxy copy.

After choosing node owners, each relationship's primary copy is placed on one of its endpoint owners. A relationship is never primarily assigned to an unrelated third database, because doing so would create unnecessary endpoint proxies.

The generated placement and optimization report are written under:

```text
data/ldbc-snb/sf1_0/proxy-minimized.placement.json
data/ldbc-snb/sf1_0/proxy-minimized.optimization.json
```

## Algorithm analyzer

`POLYGRAPH_ENABLE_ALGORITHM_ANALYZER` is a compile-time option. It defaults to `OFF`.

When disabled, analyzer call sites preprocess to no-ops and `src/analysis/AlgorithmAnalyzer.cpp` is not part of `polygraph_core`.

Build the analyzer HTTP server with:

```bash
cmake --preset http-server-analysis
cmake --build --preset http-server-analysis -j
```

Per completed BFS, the analyzer appends one JSON record. Override the output path with:

```bash
export POLYGRAPH_ALGORITHM_ANALYZER_OUTPUT=/tmp/polygraph.algorithm.jsonl
```

A record contains:

- total source/frontier node uses;
- total destination/candidate node uses;
- total physical proxy-node occurrences;
- source and destination proxy occurrences separately;
- breakdown by node role, logical type, authoritative owner backend, execution backend and proxy/non-proxy status.

### Proxy-use definition

The persistent loader materializes a Proxy endpoint when an edge stored on Neo4J, ArangoDB or PostgreSQL refers to a node whose authoritative owner is another database. Therefore the analyzer counts a proxy occurrence when a node returned/used by a persistent execution backend has an authoritative owner different from that execution backend.

Redis, MMap and GPU cache accesses do not count as physical Proxy uses because those acceleration tiers do not materialize the persistent `Proxy` vertex representation.

## Prepare all SF1 instances

From the repository root:

```bash
tools/ldbc_snb/setup_sf1_distribution_matrix.sh
```

The script:

1. builds normal and analyzer HTTP binaries;
2. computes the proxy-minimized placement;
3. prepares all five SF1 distributions with the same root seed;
4. verifies that the selected roots refer to the same logical `(type, ldbc_id)` records across all distributions;
5. stops the current SF10 storage project without deleting its volumes, because SF10 and SF1 publish the same host ports;
6. loads each distribution into its own Compose project/volumes, one at a time;
7. builds `08-no-cache` and `05-gpu20-redis15-mmap15` cache plans **after loading**, so any Neo4j database-native IDs have already been resolved.

If an existing `polygraph-sf1-0-schema-based` data volume set is present, it is preserved and reused by default. Set `REUSE_EXISTING_SCHEMA_STORAGE=0` if you deliberately want to load schema-based again. A successful new load writes a `.storage-loaded.json` marker in that distribution directory.

Useful overrides:

```bash
BUILD_BINARIES=0 LOAD_STORAGE=0 BUILD_CACHES=0 tools/ldbc_snb/setup_sf1_distribution_matrix.sh
REUSE_EXISTING_SCHEMA_STORAGE=0 tools/ldbc_snb/setup_sf1_distribution_matrix.sh
FORCE_PREPARE=1 RELOAD_STORAGE=1 tools/ldbc_snb/setup_sf1_distribution_matrix.sh
```

The Compose projects are:

```text
polygraph-sf1-0-schema-based
polygraph-sf1-0-hash-attributes
polygraph-sf1-0-label-striped-3
polygraph-sf1-0-schema-family
polygraph-sf1-0-proxy-minimized
```

## Overnight benchmark

Run:

```bash
tools/measurement/run_sf1_distribution_matrix.sh 5
```

The benchmark deliberately separates instrumentation from timing:

- **analysis phase**: analyzer-enabled server, one repetition by default;
- **timing phase**: normal server with the analyzer compiled out, five repetitions by default.

Every distribution is tested with:

- `08-no-cache` to isolate persistent placement behavior;
- `05-gpu20-redis15-mmap15` to test the same placement behind the same heterogeneous acceleration policy.

For each case, four reproducible logical Person roots are used with four BFS templates:

| Template | Depth | Direction | Purpose |
| --- | ---: | --- | --- |
| `d2-both` | 2 | both | small/local traversal |
| `d3-both` | 3 | both | medium expansion |
| `d4-both` | 4 | both | larger working set / placement stress |
| `d3-directed` | 3 | directed | direction-sensitive path |

Native whole-BFS pushdown is disabled so the distributed PolyGraph routing algorithm is measured in every case. Requests use summary responses and no predecessor materialization.

By default the storage processes are restarted between cache cases without deleting volumes, then the workload performs its own warmup round. This avoids carrying Neo4J/Arango/PostgreSQL process caches directly from the previous cache configuration.

Outputs include:

```text
status.tsv
analysis/<distribution>/<cache>/algorithm.jsonl
analysis/<distribution>/<cache>/algorithm.summary.json
analysis/<distribution>/<cache>/algorithm.summary.csv
timing-comparison.csv
algorithm-comparison.csv
```

For a detached overnight run:

```bash
OUT="measurements/sf1-distribution-overnight-$(date +%Y%m%d-%H%M%S)"
nohup systemd-inhibit \
  --what=sleep \
  --mode=block \
  --why="PolyGraph SF1 distribution matrix" \
  env OUT="$OUT" tools/measurement/run_sf1_distribution_matrix.sh 5 \
  >"$OUT.log" 2>&1 &

echo $!
```

For final thesis measurements, run multiple independent matrix repetitions or counterbalance distribution order if you want to estimate residual machine-state/order effects beyond the per-case process restart.
