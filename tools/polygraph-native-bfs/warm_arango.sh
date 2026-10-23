#!/usr/bin/env bash
set -Eeuo pipefail
SF="${SF:-1.0}"
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"; cd "$ROOT"
SF_CANON="$(python3 - "$SF" <<'PY'
from decimal import Decimal
import sys
x=format(Decimal(sys.argv[1]),'f')
if '.' not in x: x += '.0'
print(x)
PY
)"
SF_KEY="sf${SF_CANON//./_}"
DIR="data/ldbc-snb/${SF_KEY}/native-arango"
DOMAIN="$DIR/domain.json"
[[ -f "$DOMAIN" ]] || { echo "Missing $DOMAIN" >&2; exit 1; }
resolve_path(){ local p="$1"; if [[ "$p" = /* ]]; then echo "$p"; elif [[ -e "$p" ]]; then echo "$p"; elif [[ -e "$(dirname "$DOMAIN")/$p" ]]; then echo "$(dirname "$DOMAIN")/$p"; else echo "$p"; fi; }
SCHEMA="$(resolve_path "$(jq -r '.paths.schema // empty' "$DOMAIN")")"
BACKENDS="$(resolve_path "$(jq -r '.paths.backendConfig // .paths.backends // empty' "$DOMAIN")")"
[[ -f "$SCHEMA" ]] || SCHEMA="$(find "$DIR" -maxdepth 1 -name '*graph-shape*.json' -print -quit)"
[[ -f "$BACKENDS" ]] || BACKENDS="$(find "$DIR" -maxdepth 1 \( -name '*backends*.json' -o -name '*backend*.json' \) -print -quit)"
DB="$(jq -r '.backends[] | select(.kind == "ArangoDB" or .id == "ArangoDB") | .connection.database // empty' "$BACKENDS" | head -n1)"
DB="${DB:-${ARANGO_DATABASE:-university}}"
USER="${ARANGO_USER:-root}"; PASSWORD="${ARANGO_ROOT_PASSWORD:-rootpassword}"; BASE="${ARANGO_URL:-http://127.0.0.1:8529}"; CURSOR="$BASE/_db/$DB/_api/cursor"; BATCH_SIZE="${ARANGO_WARM_BATCH_SIZE:-100000}"
query(){ local q="$1"; jq -n --arg query "$q" --argjson batch "$BATCH_SIZE" '{query:$query,batchSize:$batch}' | curl -fsS -u "$USER:$PASSWORD" -H 'Content-Type: application/json' -X POST "$CURSOR" -d @-; }
drain(){ local body="$1" id more; id="$(jq -r '.id // empty' <<<"$body")"; more="$(jq -r '.hasMore // false' <<<"$body")"; while [[ "$more" == true && -n "$id" ]]; do body="$(curl -fsS -u "$USER:$PASSWORD" -H 'Content-Type: application/json' -X PUT "$CURSOR/$id")"; id="$(jq -r '.id // empty' <<<"$body")"; more="$(jq -r '.hasMore // false' <<<"$body")"; done; }
mapfile -t VERTEX_COLLECTIONS < <(jq -r '.nodes[].name' "$SCHEMA" | sort -u)
mapfile -t EDGE_COLLECTIONS < <(jq -r '.relations[].name' "$SCHEMA" | sort -u)
for c in "${VERTEX_COLLECTIONS[@]}"; do echo "[warm] vertex: $c" >&2; drain "$(query "FOR d IN \`$c\` RETURN [d._key, d.ldbc_id]")"; done
for c in "${EDGE_COLLECTIONS[@]}"; do echo "[warm] edge: $c" >&2; drain "$(query "FOR e IN \`$c\` RETURN [e._key, e._from, e._to]")"; done
echo '[warm] full scan complete.'
