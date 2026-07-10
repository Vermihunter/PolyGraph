# BFS measurement framework

This folder contains a reusable benchmark framework for comparing:

1. **Native full-replica database backends**
   - full data loaded into Neo4j, then Neo4j APOC BFS
   - full data loaded into ArangoDB, then Arango named-graph BFS
   - no native full-GPU BFS is assumed or measured

2. **Your `graph_db` implementation with full data in one backend**
   - Neo4j-only graph_db run using:
     - `storage_layer/sample-university-domain.neo.graph-shape.json`
     - `tools/generators/large-university-neo.json`
     - `config/backends.neo.json`
   - Arango-only graph_db run using:
     - `storage_layer/sample-university-domain.arango.graph-shape.json`
     - `tools/generators/large-university-arango.json`
     - `config/backends.arango.json`
   - GPU-only graph_db run using:
     - `storage_layer/sample-university-domain.gpu.graph-shape.json`
     - generated GPU-source JSON copied from `paths.data`, unless you configure a real GPU-only data file
     - `config/backends.gpu.json`

3. **Your full distributed system**
   - original schema/data placement
   - `config/backends.full.json`
   - Neo4j + ArangoDB loaded through `tools/generators/from-file-to-db.py`
   - GPU placement consumed directly from the JSON by the C++ runtime

Every run can compare its output against the reference implementation in `tools/algorithms/bfs.py`.

## Config

Copy and edit:

```bash
cp tools/measurement/configs/local.example.json tools/measurement/configs/local.json
```

The project now includes the uploaded benchmark configs under `config/` and the matching graph-shape files under `storage_layer/`.

The important sections are:

```json
{
  "paths": {
    "schema": "storage_layer/sample-university-domain.graph-shape.json",
    "data": "tools/generators/large-university.json",
    "backend_config": "config/backends.full.json"
  },
  "implementation_full": {
    "neo4j": {
      "schema": "storage_layer/sample-university-domain.neo.graph-shape.json",
      "data": "tools/generators/large-university-neo.json",
      "backend_config": "config/backends.neo.json"
    },
    "arango": {
      "schema": "storage_layer/sample-university-domain.arango.graph-shape.json",
      "data": "tools/generators/large-university-arango.json",
      "backend_config": "config/backends.arango.json"
    },
    "gpu": {
      "schema": "storage_layer/sample-university-domain.gpu.graph-shape.json",
      "data": null,
      "backend_config": "config/backends.gpu.json"
    }
  }
}
```

For GPU-only `graph_db`, `data: null` means: generate a temporary full-GPU data file in the output directory by rewriting `paths.data` sources to `GPU`. If you later create `tools/generators/large-university-gpu.json`, set that path explicitly.

## One-command run

```bash
python3 tools/measurement/run_everything.py \
  --config tools/measurement/configs/local.json \
  --output-dir tools/measurement/results/full_benchmark \
  --repetitions 5 \
  --start-services \
  --start-uuid 11a96cb7-c043-4aa7-aa35-2297e9be7f06
```

Outputs:

- `measurements.csv` — main table for analysis
- `measurements.jsonl` — detailed machine-readable event log
- `summary.json` — aggregate timing summary
- `outputs/*.csv` — BFS outputs from each system
- `reference/*.csv` — reference outputs from `tools/algorithms/bfs.py`
- `diffs/*.csv` — only written when a result differs from the reference
- `plots/*.png` — generated charts

## Run parts separately

Native full database replicas only:

```bash
python3 tools/measurement/measure_bfs.py native \
  --config tools/measurement/configs/local.json \
  --output-dir tools/measurement/results/native \
  --repetitions 5 \
  --native-backends neo4j,arango \
  --start-uuid 11a96cb7-c043-4aa7-aa35-2297e9be7f06
```

Your implementation with full data in one backend at a time:

```bash
python3 tools/measurement/measure_bfs.py implementation-full \
  --config tools/measurement/configs/local.json \
  --output-dir tools/measurement/results/implementation_full \
  --repetitions 5 \
  --implementation-backends neo4j,arango,gpu \
  --start-uuid 11a96cb7-c043-4aa7-aa35-2297e9be7f06
```

Distributed system only:

```bash
python3 tools/measurement/measure_bfs.py distributed \
  --config tools/measurement/configs/local.json \
  --output-dir tools/measurement/results/distributed \
  --repetitions 5 \
  --start-uuid 11a96cb7-c043-4aa7-aa35-2297e9be7f06
```

## Test many UUIDs

```bash
python3 tools/measurement/test_bfs_outputs.py \
  --config tools/measurement/configs/local.json \
  --output-dir tools/measurement/results/correctness \
  --start-uuid uuid1 \
  --start-uuid uuid2 \
  --start-uuid uuid3
```

or:

```bash
python3 tools/measurement/test_bfs_outputs.py \
  --config tools/measurement/configs/local.json \
  --output-dir tools/measurement/results/correctness \
  --start-uuids-file start_uuids.txt
```

The script exits with code `1` if any output differs from `tools/algorithms/bfs.py`.

## Draw plots later

```bash
python3 tools/measurement/plot_measurements.py \
  --measurements-csv tools/measurement/results/full_benchmark/measurements.csv \
  --output-dir tools/measurement/results/full_benchmark/plots
```

## Useful switches

- `--reload-each-repetition`: includes load time before every measured repetition.
- `--no-compare`: skip reference comparison.
- `--start-services`: run `docker compose up -d` from `paths.docker_compose` before measuring.
- `--dry-run`: print planned commands/actions without touching Neo4j, ArangoDB, or external binaries.
- `--native-backends neo4j,arango`: native mode has no GPU option by design.
- `--implementation-backends neo4j,arango,gpu`: select which graph_db full-placement modes to run.
