# BFS measurement framework

This folder compares three execution modes against `tools/algorithms/bfs.py`:

1. **Native database traversal**
   - Neo4j uses a named Graph Data Science projection and `gds.bfs.stream`.
   - ArangoDB uses its native graph traversal query.
2. **CPGDB with the full graph routed to one backend**
   - Neo4J-only, ArangoDB-only, or GPU-only.
3. **Distributed CPGDB**
   - The prepared graph keeps its schema-based or identity-hash placement.

All graph identifiers are numeric uint64 values. Their most-significant routing
bits identify the owning backend. The command-line option names
`--start-uuid`/`--start-uuids-file` are retained for compatibility, but their
values are now decimal or `0x`-prefixed 64-bit IDs.

## Native Neo4j ID lifecycle

Real Neo4j nodes and relationships do not have a `uuid` property. Their CPGDB
ID is formed from `id(n)`/`id(r)` plus the Neo4j routing code in the ID MSBs.
Proxy nodes still carry a signed Neo4j INTEGER property named `uuid`, preserving
the full remote uint64 bit pattern.

A resolved graph and its `*.native-id-map.jsonl` sidecar are valid only for the
Neo4j database instance produced by that load. After deleting data or recreating
volumes, load again from the unresolved source graph. `cpgdb load --reset`
performs the remapping and updates the domain manifest automatically.

## Prerequisites

```bash
python3 -m pip install -r tools/requirements.txt
python3 tools/cpgdb.py services stop --volumes
python3 tools/cpgdb.py services start
python3 tools/cpgdb.py build --preset gpu-bfs
```

Verify that GDS is installed:

```bash
curl -s -u neo4j:password \
  -H 'Content-Type: application/json' \
  http://localhost:7474/db/neo4j/tx/commit \
  --data '{"statements":[{"statement":"CALL gds.version() YIELD version RETURN version"}]}' \
  | jq
```

## Prepare separate LDBC placements

Schema-based placement uses the supplied type-to-backend mapping:

```bash
python3 tools/cpgdb.py prepare ldbc \
  --input /path/to/social_network-sf0.1-CsvBasic-LongDateFormatter \
  --sf 0.1 \
  --domain-id ldbc-snb-sf0_1-schema-based \
  --output-dir data/ldbc-snb/sf0_1/schema-based \
  --placement-config tools/ldbc_snb/placement.balanced.json \
  --root-count 100
```

Identity-hash placement omits explicit type placement and routes each entity by
its deterministic identity hash:

```bash
python3 tools/cpgdb.py prepare ldbc \
  --input /path/to/social_network-sf0.1-CsvBasic-LongDateFormatter \
  --sf 0.1 \
  --domain-id ldbc-snb-sf0_1-hash-based \
  --output-dir data/ldbc-snb/sf0_1/hash-based \
  --root-count 100
```

Load either domain. A fresh Neo4j-native load requires `--reset`:

```bash
python3 tools/cpgdb.py load data/ldbc-snb/sf0_1/schema-based/domain.json \
  --reset --batch-size 5000

python3 tools/cpgdb.py load data/ldbc-snb/sf0_1/hash-based/domain.json \
  --reset --batch-size 5000
```

## Run CPGDB and store logs/results

```bash
python3 tools/cpgdb.py run data/ldbc-snb/sf0_1/schema-based/domain.json \
  --run-id ldbc-sf0_1-schema-depth7 \
  --start-uuids-file data/ldbc-snb/sf0_1/schema-based/start_uuids.txt \
  --repetitions 3 \
  -- --max-depth 7 --direction undirected --no-predecessors
```

The wrapper resolves original generated IDs through the current native-ID map,
so the same start-ID file remains usable after a fresh Neo4j load.

## Run benchmark modes separately

Native full replicas:

```bash
python3 tools/measurement/measure_bfs.py native \
  --config tools/measurement/configs/ldbc-snb-sf0_1-schema-based.json \
  --output-dir measurements/native-schema \
  --repetitions 5 \
  --native-backends neo4j,arango \
  --start-uuids-file data/ldbc-snb/sf0_1/schema-based/start_uuids.txt
```

CPGDB full placement in each backend:

```bash
python3 tools/measurement/measure_bfs.py implementation-full \
  --config tools/measurement/configs/ldbc-snb-sf0_1-schema-based.json \
  --output-dir measurements/implementation-full-schema \
  --repetitions 5 \
  --implementation-backends neo4j,arango,gpu \
  --start-uuids-file data/ldbc-snb/sf0_1/schema-based/start_uuids.txt
```

Distributed placement:

```bash
python3 tools/measurement/measure_bfs.py distributed \
  --config tools/measurement/configs/ldbc-snb-sf0_1-schema-based.json \
  --output-dir measurements/distributed-schema \
  --repetitions 5 \
  --start-uuids-file data/ldbc-snb/sf0_1/schema-based/start_uuids.txt
```

## Correctness and plots

```bash
python3 tools/measurement/test_bfs_outputs.py \
  --config tools/measurement/configs/ldbc-snb-sf0_1-schema-based.json \
  --output-dir measurements/correctness-schema \
  --start-uuids-file data/ldbc-snb/sf0_1/schema-based/start_uuids.txt

python3 tools/measurement/plot_measurements.py \
  --measurements-csv measurements/distributed-schema/measurements.csv \
  --output-dir measurements/distributed-schema/plots
```

Useful switches include `--reload-each-repetition`, `--no-compare`,
`--start-services`, and `--dry-run`.
