# LDBC preparation

## Load

```shell
python3 tools/cpgdb.py load data/ldbc-snb/sf0_1/hash-based/domain.json 
```

## Preparation

### Extra arguments

- `-seed` is not needed for schema-based placement
  - It only makes random operations reproducible, mainly selecting the start_uuids.txt roots and any randomized data generation
  - Backend placement by type does not use it
- `-start-type` 
  - During prepare ldbc, it only filters which node type is sampled for the generated starting UUID file, for example choosing five `Person` nodes

### Path to the dataset

Set the `RAW` environment variable to the path of the raw data:
```shell
RAW=/path/to/social_network-sf0.1-CsvBasic-LongDateFormatter
```


## Schema-based placement

```shell
python3 tools/cpgdb.py prepare ldbc \
  --input "$RAW" \
  --sf 0.1 \
  --domain-id ldbc-snb-sf0_1-schema-based \
  --placement-config data/ldbc-snb/placement.schema.json \
  --storage-backends Neo4J ArangoDB GPU \
  --arango-database ldbc_snb_sf0_1_schema \
  --start-type Person \
  --root-count 5 \
  --seed 42
  
mkdir -p data/ldbc-snb/sf0_1/schema-based
mv data/ldbc-snb/sf0_1/*.json data/ldbc-snb/sf0_1/*.txt data/ldbc-snb/sf0_1/*.csv data/ldbc-snb/sf0_1/*.jsonl data/ldbc-snb/sf0_1/schema-based
```

## Hash-based placement

```shell
python3 tools/cpgdb.py prepare ldbc \
  --input "$RAW" \
  --sf 0.1 \
  --domain-id ldbc-snb-sf0_1-hash-based \
  --storage-backends Neo4J ArangoDB GPU \
  --arango-database ldbc_snb_sf0_1_hash \
  --start-type Person \
  --root-count 5 \
  --seed 42

mkdir -p data/ldbc-snb/sf0_1/hash-based
mv data/ldbc-snb/sf0_1/*.json data/ldbc-snb/sf0_1/*.txt data/ldbc-snb/sf0_1/*.csv data/ldbc-snb/sf0_1/*.jsonl data/ldbc-snb/sf0_1/hash-based
```