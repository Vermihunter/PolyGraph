#!/usr/bin/env bash
set -u

SF="${SF:-1.0}"

SF_CANON="$(
python3 - "$SF" <<'PY'
from decimal import Decimal
import sys
x=format(Decimal(sys.argv[1]),"f")
if "." not in x: x += ".0"
print(x)
PY
)"
SF_KEY="sf${SF_CANON//./_}"

CONTAINER="$(
    docker ps -a --format '{{.Names}}' |
    grep -E "polygraph-${SF_KEY//_/-}-schema-based-arangodb-1|schema-based-arangodb" |
    head -n1
)"

echo "=== Host memory ==="
free -h

echo
echo "=== Container ==="
echo "${CONTAINER:-not found}"

if [[ -n "$CONTAINER" ]]; then
    docker inspect "$CONTAINER" \
      --format 'status={{.State.Status}}
oomKilled={{.State.OOMKilled}}
exitCode={{.State.ExitCode}}
restartCount={{.RestartCount}}
dockerMemoryLimitBytes={{.HostConfig.Memory}}'

    echo
    echo "=== Arango environment ==="
    docker inspect "$CONTAINER" \
      --format '{{range .Config.Env}}{{println .}}{{end}}' |
      grep -Ei 'ARANGO|MEMORY|CACHE|ROCKS|QUERY' || true

    echo
    echo "=== Current container usage ==="
    docker stats --no-stream "$CONTAINER" || true

    echo
    echo "=== Recent logs ==="
    docker logs --tail 120 "$CONTAINER" || true
fi

echo
echo "=== Kernel OOM messages (may require sudo) ==="
journalctl -k --since '-10 min' 2>/dev/null |
grep -Ei 'out of memory|oom|killed process|arangod' || true
