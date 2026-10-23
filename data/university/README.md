# Dataset generation

Remember to update the "schema" field in the generated `domain.json` file to a relative "../schema/.." Path

## Loading the dataset

```shell
python3 tools/cpgdb.py services stop --volumes
python3 tools/cpgdb.py services start
python3 tools/cpgdb.py load data/university/schema-based/domain.json
```

## Running

```shell

```

## Identity hash-based


```shell
python3 tools/cpgdb.py prepare university \
  --input data/university/full/large-university-orig.json \
  --schema data/university/schema/sample-university-domain.graph-shape.json \
  --placement-strategy identity-hash
  
mkdir -p data/university/hash-based
mv data/university/domain.json \
  data/university/graph.json \
  data/university/backends.json \
  data/university/validation.json \
  data/university/hash-based 
```

## Schema-based

```shell
python3 tools/cpgdb.py prepare university  \
  --input data/university/full/large-university-orig.json  \
  --schema data/university/schema/sample-university-domain.graph-shape.json  \
  --placement-strategy schema

mkdir -p data/university/schema-based
mv data/university/domain.json \
  data/university/graph.json \
  data/university/backends.json \
  data/university/validation.json \
  data/university/schema-based 
```