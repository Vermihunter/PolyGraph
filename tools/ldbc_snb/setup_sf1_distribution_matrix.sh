#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

PYTHON_BIN="${PYTHON_BIN:-python3}"
RAW="${RAW:-data/ldbc-snb/sf1_0/raw}"
ROOT_COUNT="${ROOT_COUNT:-100}"
ROOT_SEED="${ROOT_SEED:-42}"
LOAD_BATCH_SIZE="${LOAD_BATCH_SIZE:-20000}"
LOAD_WORKERS="${LOAD_WORKERS:-4}"
PREPARE="${PREPARE:-1}"
FORCE_PREPARE="${FORCE_PREPARE:-0}"
BUILD_CACHES="${BUILD_CACHES:-1}"
LOAD_STORAGE="${LOAD_STORAGE:-1}"
RELOAD_STORAGE="${RELOAD_STORAGE:-0}"
BUILD_BINARIES="${BUILD_BINARIES:-1}"
BUILD_JOBS="${BUILD_JOBS:-2}"
STOP_SF10="${STOP_SF10:-1}"
REUSE_EXISTING_SCHEMA_STORAGE="${REUSE_EXISTING_SCHEMA_STORAGE:-1}"

DISTRIBUTIONS=(
  schema-based
  hash-attributes
  label-striped-3
  schema-family
  proxy-minimized
)

CACHE_CONFIGS=(
  config/cache/benchmark/sf1/08-no-cache.json
  config/cache/benchmark/sf1/05-gpu20-redis15-mmap15.json
)

log() { printf '\n============================================================\n%s\n============================================================\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[[ -d "$RAW" ]] || die "Missing SF1 raw dataset: $RAW"
[[ -f tools/polygraph.py ]] || die "Missing tools/polygraph.py"

project_for() {
  printf 'polygraph-sf1-0-%s' "$1"
}

stop_project() {
  local distribution="$1"
  docker compose \
    --env-file storage_layer/.env \
    -f storage_layer/docker-compose.yml \
    --project-name "$(project_for "$distribution")" \
    stop >/dev/null 2>&1 || true
}

stop_all_matrix_projects() {
  local distribution
  for distribution in "${DISTRIBUTIONS[@]}"; do
    stop_project "$distribution"
  done
}

stop_sf10_schema_project() {
  docker compose \
    --env-file storage_layer/.env \
    -f storage_layer/docker-compose.yml \
    --project-name polygraph-sf10-0-schema-based \
    stop >/dev/null 2>&1 || true
}

project_has_data_volumes() {
  local project="$1"
  docker volume inspect \
    "${project}_neo4j_data" \
    "${project}_arangodb_data" \
    "${project}_postgresql_data" >/dev/null 2>&1
}

placement_for() {
  case "$1" in
    schema-based)      echo "tools/ldbc_snb/placement.schema-3db.json" ;;
    hash-attributes)   echo "tools/ldbc_snb/placement.hash-attributes-3db.json" ;;
    label-striped-3)   echo "tools/ldbc_snb/placement.label-striped-3db.json" ;;
    schema-family)     echo "tools/ldbc_snb/placement.schema-family-3db.json" ;;
    proxy-minimized)   echo "data/ldbc-snb/sf1_0/proxy-minimized.placement.json" ;;
    *) die "Unknown distribution: $1" ;;
  esac
}

if [[ "$BUILD_BINARIES" == "1" ]]; then
  log "BUILDING NORMAL + ANALYZER HTTP SERVERS"
  cmake --preset http-server
  cmake --build --preset http-server -j"$BUILD_JOBS"
  cmake --preset http-server-analysis
  cmake --build --preset http-server-analysis -j"$BUILD_JOBS"
fi

if [[ "$PREPARE" == "1" ]]; then
  log "OPTIMIZING PROXY-MINIMIZED SF1 PLACEMENT"
  "$PYTHON_BIN" tools/ldbc_snb/optimize_proxy_placement.py \
    --input "$RAW" \
    --output data/ldbc-snb/sf1_0/proxy-minimized.placement.json \
    --report data/ldbc-snb/sf1_0/proxy-minimized.optimization.json

  for distribution in "${DISTRIBUTIONS[@]}"; do
    output="data/ldbc-snb/sf1_0/$distribution"
    placement="$(placement_for "$distribution")"

    if [[ "$FORCE_PREPARE" != "1" && -f "$output/domain.json" ]]; then
      echo "[setup] $distribution already prepared; keeping $output/domain.json"
      continue
    fi

    log "PREPARING SF1 DISTRIBUTION: $distribution"
    if [[ "$FORCE_PREPARE" == "1" ]]; then
      rm -rf "$output"
    fi
    mkdir -p "$output"

    "$PYTHON_BIN" tools/polygraph.py prepare ldbc \
      --input "$RAW" \
      --sf 1.0 \
      --domain-id "ldbc-snb-sf1_0-$distribution" \
      --output-dir "$output" \
      --placement-config "$placement" \
      --storage-backends Neo4J ArangoDB PostgreSQL \
      --root-count "$ROOT_COUNT" \
      --seed "$ROOT_SEED" \
      --validation-mode basic
  done

  log "VERIFYING IDENTICAL LOGICAL BFS ROOTS"
  "$PYTHON_BIN" - <<'PY'
import csv
from pathlib import Path

names = ["schema-based", "hash-attributes", "label-striped-3", "schema-family", "proxy-minimized"]
expected = None
for name in names:
    path = Path("data/ldbc-snb/sf1_0") / name / "bfs_roots.csv"
    with path.open(newline="", encoding="utf-8") as handle:
        logical = [(row.get("type", ""), row.get("ldbc_id", "")) for row in csv.DictReader(handle)]
    if expected is None:
        expected = logical
    elif logical != expected:
        raise SystemExit(f"Logical BFS roots differ for {name}: {path}")
    print(f"{name:18s}: {len(logical):3d} roots; semantic IDs match")
PY
fi

if [[ "$LOAD_STORAGE" == "1" ]]; then
  if [[ "$STOP_SF10" == "1" ]]; then
    log "STOPPING THE CURRENT SF10 STORAGE PROJECT (VOLUMES PRESERVED)"
    stop_sf10_schema_project
  fi
  if [[ "$FORCE_PREPARE" == "1" && "$RELOAD_STORAGE" != "1" ]]; then
    die "FORCE_PREPARE=1 changes encoded graph IDs; use RELOAD_STORAGE=1 so old database volumes are not reused"
  fi

  # All distributions use the same published host ports. Keep their volumes, but
  # load/activate only one Compose project at a time.
  stop_all_matrix_projects

  for distribution in "${DISTRIBUTIONS[@]}"; do
    domain="data/ldbc-snb/sf1_0/$distribution/domain.json"
    marker="data/ldbc-snb/sf1_0/$distribution/.storage-loaded.json"
    project="$(project_for "$distribution")"

    if [[ "$RELOAD_STORAGE" != "1" && -f "$marker" ]]; then
      echo "[setup] $distribution already has a storage marker; skipping load"
      continue
    fi
    if [[ "$distribution" == "schema-based" && "$REUSE_EXISTING_SCHEMA_STORAGE" == "1" \
          && "$RELOAD_STORAGE" != "1" && "$FORCE_PREPARE" != "1" \
          && -f "$domain" ]] && project_has_data_volumes "$project"; then
      echo "[setup] reusing existing schema-based SF1 volumes; set REUSE_EXISTING_SCHEMA_STORAGE=0 to reload them"
      printf '{"distribution":"%s","loadedAt":"%s","assumedExisting":true}\n' \
        "$distribution" "$(date --iso-8601=seconds)" > "$marker"
      continue
    fi

    log "LOADING PERSISTENT INSTANCE: $distribution"
    stop_all_matrix_projects

    if [[ "$RELOAD_STORAGE" == "1" ]]; then
      # Remove only this distribution's containers/volumes. Never touch the raw
      # dataset or prepared domain.
      docker compose \
        --env-file storage_layer/.env \
        -f storage_layer/docker-compose.yml \
        --project-name "$project" \
        down --volumes --remove-orphans || true
      rm -f "$marker"
    fi

    "$PYTHON_BIN" tools/polygraph.py instance 1 "$distribution" load "$domain" \
      --batch-size "$LOAD_BATCH_SIZE" \
      --workers "$LOAD_WORKERS" \
      --postgres-indexes

    printf '{"distribution":"%s","loadedAt":"%s"}\n' \
      "$distribution" "$(date --iso-8601=seconds)" > "$marker"
    stop_project "$distribution"
  done
fi

# Build cache artifacts only after persistent loading. Neo4j uses database-native
# IDs, and the load step resolves the prepared domain to those IDs.
if [[ "$BUILD_CACHES" == "1" ]]; then
  for distribution in "${DISTRIBUTIONS[@]}"; do
    domain="data/ldbc-snb/sf1_0/$distribution/domain.json"
    cache_root="data/ldbc-snb/sf1_0/$distribution/caches"
    planning_db="data/ldbc-snb/sf1_0/$distribution/cache-plan.sqlite3"
    [[ -f "$domain" ]] || die "Missing prepared domain: $domain"

    log "BUILDING CACHE PLANS: $distribution"
    "$PYTHON_BIN" tools/polygraph.py cache build "$domain" \
      --config "${CACHE_CONFIGS[@]}" \
      --output-dir "$cache_root" \
      --planning-db "$planning_db"
  done
fi

log "SF1 DISTRIBUTION MATRIX READY"
printf 'Prepared distributions:\n'
for distribution in "${DISTRIBUTIONS[@]}"; do
  printf '  %-18s %s\n' "$distribution" "data/ldbc-snb/sf1_0/$distribution/domain.json"
done
printf '\nBenchmark with:\n  tools/measurement/run_sf1_distribution_matrix.sh 5\n'
