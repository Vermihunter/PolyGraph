OUT="measurements/sf1-cache-N10-no-prefill"
N=10

rm -rf "$OUT"
mkdir -p "$OUT"

export ARANGO_ROCKSDB_AUTO_FILL_INDEX_CACHES_ON_STARTUP=false

for CFG in config/cache/benchmark/sf1/*.json; do
    NAME="$(basename "$CFG" .json)"

    echo
    echo "============================================================"
    echo "CACHE: $NAME"
    echo "============================================================"

    # 1. Fresh Arango process for every cache configuration.
    #    Database/index volumes are PRESERVED.
    #    Auto-fill is OFF.
    docker compose \
      --env-file storage_layer/.env \
      -p polygraph-sf1-0-schema-based \
      -f storage_layer/docker-compose.yml \
      -f storage_layer/docker-compose.arango-bfs-warm.yml \
      up -d --force-recreate --no-deps arangodb

    # 2. Wait until Arango answers requests.
    until curl -sf \
      -u root:rootpassword \
      http://127.0.0.1:8529/_api/version \
      >/dev/null; do
        sleep 0.5
    done

    # Verify the important setting.
    ARANGO="$(docker ps --format '{{.Names}}' | grep -i arango | head -1)"

    docker inspect "$ARANGO" --format '{{json .Config.Cmd}}' |
      jq -r '.[]' |
      grep 'auto-fill-index-caches'

    # 3. Run this cache configuration.
    #
    # benchmark_compact_repeated.py gives:
    #   - one unmeasured warmup ROUND
    #   - N measured repetitions
    #   - no cache planning
    TMP="$OUT/.tmp-$NAME"

    python3 tools/measurement/benchmark_compact_repeated.py \
      --domain data/ldbc-snb/sf1_0/schema-based/domain.json \
      --cache-configs "$CFG" \
      --cache-root data/ldbc-snb/sf1_0/schema-based/caches \
      --request-plan config/benchmarks/sf1-cache-http-requests.example.json \
      --repetitions "$N" \
      --output-dir "$TMP" \
      --overwrite

    # 4. Flatten the result.
    #
    # From:
    #   .tmp-04.../04.../events/...
    #
    # To:
    #   OUT/04.../events/...
    mv "$TMP/$NAME" "$OUT/$NAME"

    # Keep the generated request plan once for reproducibility.
    if [[ ! -d "$OUT/_benchmark" ]]; then
        mv "$TMP/_benchmark" "$OUT/_benchmark"
    fi

    rm -rf "$TMP"
done