# CPGDB University and LDBC SNB Quick Start

Run all commands from the repository root.

## Common setup

```bash
git clone <repository-url> vermesa
cd vermesa

python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/pip install -r tools/requirements.txt

PYTHON="$PWD/.venv/bin/python"
```

Build the GPU-capable BFS executable once. The same binary can run both domains:

```bash
$PYTHON tools/cpgdb.py build --preset gpu-bfs
```

## University: prepare and run from zero

Generate the canonical `graph.jsonl`, backend configuration, validation report, domain manifest, and measurement configuration from the existing University JSON:

```bash
$PYTHON tools/cpgdb.py prepare university   --input data/university/full/large-university-orig.json
```

Generated files include:

```text
data/university/graph.jsonl
data/university/backends.json
data/university/domain.json
data/university/validation.json
tools/measurement/configs/university.json
```

Start clean databases, load the graph, and run BFS:

```bash
$PYTHON tools/cpgdb.py services stop --volumes
$PYTHON tools/cpgdb.py services start --timeout 300

$PYTHON tools/cpgdb.py load university --batch-size 500
$PYTHON tools/cpgdb.py run university
```

Run from an explicit vertex:

```bash
$PYTHON tools/cpgdb.py run university --   --start-uuid "11a96cb7-c043-4aa7-aa35-2297e9be7f06"   --start-type Faculty   --max-depth 7
```

## LDBC SNB SF0.1: prepare and run from zero

Convert both the `static/` and `dynamic/` CSV directories:

```bash
$PYTHON tools/cpgdb.py prepare ldbc   --input data/ldbc-snb/sf0_1/raw/social_network-sf0.1-CsvBasic-LongDateFormatter   --sf 0.1
```

To apply a custom backend placement, add:

```bash
--placement-config tools/ldbc_snb/placement.example.json
```

Generated files include:

```text
data/ldbc-snb/sf0_1/graph.jsonl
data/ldbc-snb/sf0_1/backends.json
data/ldbc-snb/sf0_1/domain.json
data/ldbc-snb/sf0_1/ldbc-snb.graph-shape.json
data/ldbc-snb/sf0_1/start_uuids.txt
data/ldbc-snb/sf0_1/validation.json
tools/measurement/configs/ldbc-snb-sf0_1.json
```

Replace the currently loaded graph, load LDBC, and run BFS:

```bash
$PYTHON tools/cpgdb.py services stop --volumes
$PYTHON tools/cpgdb.py services start --timeout 300

$PYTHON tools/cpgdb.py load ldbc-snb-sf0_1 --batch-size 500
$PYTHON tools/cpgdb.py run ldbc-snb-sf0_1
```

Run from the first generated Person root:

```bash
START_UUID="$(head -n 1 data/ldbc-snb/sf0_1/start_uuids.txt)"

$PYTHON tools/cpgdb.py run ldbc-snb-sf0_1 --   --start-uuid "$START_UUID"   --start-type Person   --max-depth 7   --direction undirected
```

## Measurements repeated N times

The measurement runner loads the selected graph once, then runs every requested start UUID `N` times. Add `--reload-each-repetition` to include loading before every repetition.

### University: one root repeated N times

```bash
N=5
START_UUID="$(jq -r '.defaults.bfs.startUuid' data/university/domain.json)"

$PYTHON tools/measurement/measure_bfs.py distributed   --config tools/measurement/configs/university.json   --output-dir tools/measurement/results/university   --start-uuid "$START_UUID"   --repetitions "$N"   --no-compare
```

### LDBC: every UUID in the file repeated N times

Use the five-root file:

```bash
N=5

$PYTHON tools/measurement/measure_bfs.py distributed   --config tools/measurement/configs/ldbc-snb-sf0_1.json   --output-dir tools/measurement/results/ldbc-snb-sf0_1   --start-uuids-file data/ldbc-snb/sf0_1/start_uuids_5.txt   --repetitions "$N"   --no-compare
```

Use all sampled roots by replacing the file with:

```text
data/ldbc-snb/sf0_1/start_uuids.txt
```

Total BFS executions are:

```text
number of UUIDs × N
```

Results are written to the selected output directory:

```text
measurements.csv
measurements.jsonl
summary.json
```


python3 tools/cpgdb.py benchmark   config/benchmarks/ldbc-hash-existing-distributed-reference.json   --run-id ldbc-hash-distributed