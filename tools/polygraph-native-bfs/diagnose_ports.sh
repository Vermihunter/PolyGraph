#!/usr/bin/env bash
set -u

echo '=== Docker containers ==='
docker ps --format 'table {{.Names}}\t{{.Status}}\t{{.Ports}}' || true

echo
echo '=== Relevant listeners ==='
if command -v ss >/dev/null 2>&1; then
  ss -ltnp 2>/dev/null | grep -E ':(7474|7687|2004|8529)\b' || true
fi

echo
echo '=== API probes ==='
printf 'Neo4j 7474: '
if curl -fsS http://127.0.0.1:7474/ >/dev/null 2>&1; then echo UP; else echo DOWN; fi
printf 'Arango 8529: '
if curl -fsS -u "${ARANGO_USER:-root}:${ARANGO_ROOT_PASSWORD:-rootpassword}" http://127.0.0.1:8529/_api/version >/dev/null 2>&1; then echo UP; else echo 'DOWN / auth mismatch'; fi
