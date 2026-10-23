#!/usr/bin/env bash
set -Eeuo pipefail

BACKEND="${1:?Usage: $0 neo4j|arango}"
SF="${SF:-1.0}"
FORCE_PREPARE="${FORCE_PREPARE:-0}"
START_DB="${START_DB:-auto}"
LOAD_DATA="${LOAD_DATA:-1}"
BATCH_SIZE="${BATCH_SIZE:-5000}"
INSTANCE_DISTRIBUTION="${INSTANCE_DISTRIBUTION:-schema-based}"

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT"

for cmd in python3 curl jq; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "Missing required command: $cmd" >&2; exit 1; }
done

SF_CANON="$(python3 - "$SF" <<'PY'
from decimal import Decimal
import sys
x = format(Decimal(sys.argv[1]), 'f')
if '.' not in x:
    x += '.0'
print(x)
PY
)"
SF_KEY="sf${SF_CANON//./_}"

case "$BACKEND" in
    neo4j)
        SOURCE="Neo4J"
        LAYOUT="native-neo4j"
        API_URL="${NEO4J_HTTP_URL:-http://127.0.0.1:7474/}"
        ;;
    arango)
        SOURCE="ArangoDB"
        LAYOUT="native-arango"
        API_URL="${ARANGO_URL:-http://127.0.0.1:8529}/_api/version"
        ;;
    *)
        echo "Backend must be neo4j or arango" >&2
        exit 2
        ;;
esac

RAW_DIR="${RAW_DIR:-data/ldbc-snb/${SF_KEY}/raw}"
OUT_DIR="${OUT_DIR:-data/ldbc-snb/${SF_KEY}/${LAYOUT}}"
DOMAIN_ID="${DOMAIN_ID:-ldbc-snb-${SF_KEY}-${LAYOUT}}"
DOMAIN="${OUT_DIR}/domain.json"
GRAPH="${OUT_DIR}/graph.jsonl"
ROOT_COUNT="${ROOT_COUNT:-100}"

backend_ready() {
    case "$BACKEND" in
        neo4j)
            curl -fsS "$API_URL" >/dev/null 2>&1
            ;;
        arango)
            curl -fsS -u "${ARANGO_USER:-root}:${ARANGO_ROOT_PASSWORD:-rootpassword}" "$API_URL" >/dev/null 2>&1
            ;;
    esac
}

resolve_manifest_path() {
    local expr="$1"
    local raw
    raw="$(jq -r "$expr // empty" "$DOMAIN")"
    [[ -n "$raw" ]] || return 1

    if [[ "$raw" = /* && -f "$raw" ]]; then
        printf '%s\n' "$raw"
    elif [[ -f "$raw" ]]; then
        realpath "$raw"
    elif [[ -f "$(dirname "$DOMAIN")/$raw" ]]; then
        realpath "$(dirname "$DOMAIN")/$raw"
    else
        return 1
    fi
}

echo "============================================================"
echo "PolyGraph native full-replica storage baseline"
echo "============================================================"
echo "SF:                    $SF_CANON"
echo "backend:               $BACKEND"
echo "native placement:      ALL -> $SOURCE"
echo "artifact directory:    $OUT_DIR"
echo "instance distribution: $INSTANCE_DISTRIBUTION"
echo "============================================================"

# 1. Prepare a full-replica graph description for exactly one backend.
if [[ "$FORCE_PREPARE" != "1" && -f "$DOMAIN" && -f "$GRAPH" ]]; then
    echo "[native] Existing native domain found; skipping conversion: $DOMAIN"
else
    [[ -d "$RAW_DIR" ]] || { echo "Missing raw dataset: $RAW_DIR" >&2; exit 1; }

    mkdir -p "$OUT_DIR"
    PLACEMENT="$(mktemp)"
    trap 'rm -f "$PLACEMENT"' EXIT

    cat > "$PLACEMENT" <<JSON
{
  "defaultNodeSource": "$SOURCE",
  "defaultRelationshipSource": "$SOURCE",
  "nodeSources": {},
  "relationshipSources": {}
}
JSON

    echo "[native] Preparing 100% of the graph for $SOURCE..."
    python3 tools/polygraph.py prepare ldbc \
        --input "$RAW_DIR" \
        --sf "$SF_CANON" \
        --domain-id "$DOMAIN_ID" \
        --output-dir "$OUT_DIR" \
        --placement-config "$PLACEMENT" \
        --root-count "$ROOT_COUNT"
fi

# 2. Use the normal schema-based instance/container set.
case "$START_DB" in
    never)
        echo "[native] START_DB=never: reusing the existing schema-based instance."
        ;;
    auto)
        if backend_ready; then
            echo "[native] $BACKEND is already reachable; reusing the schema-based instance."
        else
            echo "[native] Starting PolyGraph instance: $SF_KEY $INSTANCE_DISTRIBUTION"
            python3 tools/polygraph.py instance "$SF_KEY" "$INSTANCE_DISTRIBUTION" up
        fi
        ;;
    always)
        python3 tools/polygraph.py instance "$SF_KEY" "$INSTANCE_DISTRIBUTION" up
        ;;
    *)
        echo "START_DB must be auto, always, or never" >&2
        exit 2
        ;;
esac

for _ in $(seq 1 120); do
    backend_ready && break
    sleep 1
done
backend_ready || { echo "$BACKEND did not become reachable at $API_URL" >&2; exit 1; }

# 3. Load the native full replica directly through the lower-level loader.
#
# Do NOT run:
#   polygraph.py instance sfX schema-based load
# because that resolves data/ldbc-snb/sfX/schema-based/domain.json and would load
# the normal distributed placement rather than OUT_DIR/graph.jsonl.
if [[ "$LOAD_DATA" == "1" ]]; then
    BACKENDS="$(resolve_manifest_path '.paths.backendConfig' || resolve_manifest_path '.paths.backends' || true)"

    if [[ -z "$BACKENDS" || ! -f "$BACKENDS" ]]; then
        BACKENDS="$(find "$OUT_DIR" -maxdepth 1 -type f \( -name '*backends*.json' -o -name '*backend*.json' \) -print -quit)"
    fi

    [[ -f "$BACKENDS" ]] || { echo "Could not resolve backend config for $DOMAIN" >&2; exit 1; }

    echo
    echo "WARNING: resetting selected backend $SOURCE"
    echo "         its existing schema-based contents will be replaced"
    echo "         by the COMPLETE LDBC graph."
    echo
    echo "[native] graph:    $GRAPH"
    echo "[native] backends: $BACKENDS"
    echo "[native] source:   $SOURCE"

    # The graph IDs were generated with the original routing layout
    # (currently Neo4J, ArangoDB, GPU).  load_graph.py validates that every
    # routing slot is enabled before it applies --only-source.  The generated
    # native backends.json may disable GPU because this is a storage-only
    # domain, which makes the validator reject the otherwise valid graph.
    #
    # Do NOT shrink/reorder idRouting here: that would change the meaning of
    # already encoded IDs (especially ArangoDB, which occupies routing slot 1).
    #
    # Instead create a loader-only config that keeps the exact routing layout
    # and marks routing-only entries enabled.  read_backend_connections()
    # ignores non-storage backends such as GPU, and --only-source still limits
    # the actual load to Neo4J or ArangoDB.
    LOADER_BACKENDS="$(mktemp)"
    trap 'rm -f "${PLACEMENT:-}" "${LOADER_BACKENDS:-}"' EXIT

    python3 - "$BACKENDS" "$LOADER_BACKENDS" <<'PY'
import json
import sys
from pathlib import Path

src = Path(sys.argv[1])
dst = Path(sys.argv[2])

cfg = json.loads(src.read_text(encoding="utf-8"))
routes = list(cfg.get("idRouting", []))
entries = cfg.setdefault("backends", [])

by_id = {
    str(entry.get("id")): entry
    for entry in entries
    if isinstance(entry, dict) and entry.get("id") is not None
}

for route in routes:
    entry = by_id.get(route)
    if entry is not None:
        entry["enabled"] = True
        continue

    # Routing-only placeholder.  The Python graph loader only creates real
    # connections for Neo4J/ArangoDB, so a GPU placeholder is never contacted.
    if route == "GPU":
        entry = {
            "id": "GPU",
            "kind": "CuGraph",
            "enabled": True,
            "name": "GPU routing slot (loader-only)",
            "connection": {"endpoint": "", "database": ""},
            "capabilities": {"gpu_execution": True},
            "coverage": {"mode": "id-routing"},
        }
        entries.append(entry)
        by_id[route] = entry
    else:
        raise SystemExit(
            f"Routing backend {route!r} is missing from {src}; "
            "refusing to invent a storage connection."
        )

# This temporary file is only for graph ingestion; it must not require CUDA.
cfg["require_cuda_for_gpu"] = False
dst.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
PY

    echo "[native] loader config: $LOADER_BACKENDS"
    echo "[native] preserving ID routing: $(jq -c '.idRouting' "$LOADER_BACKENDS")"

    python3 tools/load_graph.py \
        "$GRAPH" \
        --backend-config "$LOADER_BACKENDS" \
        --only-source "$SOURCE" \
        --reset \
        --batch-size "$BATCH_SIZE"
fi

echo
echo "DONE"
echo "instance: $SF_KEY / $INSTANCE_DISTRIBUTION"
echo "backend:  $SOURCE"
echo "graph:    $GRAPH"
