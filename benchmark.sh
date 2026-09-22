set -euo pipefail

OUT="measurements/sf1-cache-N10-no-prefill"
N=10

# Important: keep Arango global index prefill disabled.
export ARANGO_ROCKSDB_AUTO_FILL_INDEX_CACHES_ON_STARTUP=false


CFG=config/cache/benchmark/sf1/01-gpu-100.json
NAME="$(basename "$CFG" .json)"
TMP="$OUT/.tmp-$NAME"

echo
echo "============================================================"
echo "CACHE: $NAME"
echo "============================================================"

# Fresh Arango process for this configuration.
# Volumes/data/indexes are preserved.
docker compose \
  --env-file storage_layer/.env \
  -p polygraph-sf1-0-schema-based \
  -f storage_layer/docker-compose.yml \
  -f storage_layer/docker-compose.arango-bfs-warm.yml \
  up -d --force-recreate --no-deps arangodb

# Wait for Arango.
until curl -sf \
  -u root:rootpassword \
  http://127.0.0.1:8529/_api/version \
  >/dev/null
do
    sleep 0.5
done

# Sanity check: MUST print "...=false".
ARANGO="$(docker ps --format '{{.Names}}' | grep -i arango | head -1)"

docker inspect "$ARANGO" --format '{{json .Config.Cmd}}' |
  jq -r '.[]' |
  grep 'auto-fill-index-caches'

# Clean possible failed staging output from previous attempt.
rm -rf "$TMP"

# One warmup round + N measured repetitions.
python3 tools/measurement/benchmark_compact_repeated.py \
  --domain data/ldbc-snb/sf1_0/schema-based/domain.json \
  --cache-configs "$CFG" \
  --cache-root data/ldbc-snb/sf1_0/schema-based/caches \
  --request-plan config/benchmarks/sf1-cache-http-requests.example.json \
  --repetitions "$N" \
  --http-port 8080 \
  --output-dir "$TMP" \
  --overwrite

# Move the completed flat configuration directory into final results.
rm -rf "$OUT/$NAME"
mv "$TMP/$NAME" "$OUT/$NAME"

# Keep one copy of benchmark metadata/request plan.
if [[ ! -d "$OUT/_benchmark" && -d "$TMP/_benchmark" ]]; then
    mv "$TMP/_benchmark" "$OUT/_benchmark"
fi

rm -rf "$TMP"
