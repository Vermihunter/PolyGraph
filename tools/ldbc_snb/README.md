# LDBC SNB tools

The converter reads the initial snapshot from sibling `static/` and `dynamic/`
directories. Top-level `updateStream_*.csv` files are intentionally ignored for
the read-only BFS workload.

## Schema-based placement

```bash
python3 tools/cpgdb.py prepare ldbc \
  --input /path/to/social_network-sf0.1-CsvBasic-LongDateFormatter \
  --sf 0.1 \
  --domain-id ldbc-snb-sf0_1-schema-based \
  --output-dir data/ldbc-snb/sf0_1/schema-based \
  --placement-config tools/ldbc_snb/placement.balanced.json \
  --storage-backends Neo4J ArangoDB GPU
```

## Identity-hash placement

```bash
python3 tools/cpgdb.py prepare ldbc \
  --input /path/to/social_network-sf0.1-CsvBasic-LongDateFormatter \
  --sf 0.1 \
  --domain-id ldbc-snb-sf0_1-hash-based \
  --output-dir data/ldbc-snb/sf0_1/hash-based \
  --storage-backends Neo4J ArangoDB GPU
```

Without `--placement-config`, each node and relationship is assigned using a
stable identity hash. In both variants, IDs are uint64 values with backend
routing codes in their most-significant bits.

## Official Datagen

```bash
python3 tools/cpgdb.py generate ldbc --sf 0.1
```

`Dockerfile.datagen` supplies Java 8 and Hadoop 3.2.1 for the pinned Datagen
launcher.

## Load and run

```bash
python3 tools/cpgdb.py services start
python3 tools/cpgdb.py build --preset gpu-bfs
python3 tools/cpgdb.py load data/ldbc-snb/sf0_1/schema-based/domain.json \
  --reset --batch-size 5000
python3 tools/cpgdb.py run data/ldbc-snb/sf0_1/schema-based/domain.json \
  --run-id ldbc-schema \
  --start-uuids-file data/ldbc-snb/sf0_1/schema-based/start_uuids.txt \
  --repetitions 3 \
  -- --max-depth 7 --direction undirected
```

The compatibility-named `start_uuids.txt` contains decimal uint64 IDs. After a
Neo4j reset, the loader creates a new resolved graph and native-ID map; the run
wrapper remaps the original start IDs automatically.
