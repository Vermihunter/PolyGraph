#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from pathlib import Path
from typing import Iterator

ROOT = Path(__file__).resolve().parents[2]

INDEX_RECORD = struct.Struct("<QQQ")
UINT64 = struct.Struct("<Q")


def _redis_backend(backends_path: Path) -> dict:
    raw = json.loads(backends_path.read_text(encoding="utf-8"))
    for backend in raw.get("backends", []):
        if backend.get("enabled", True) and str(backend.get("id")) == "Redis":
            return backend
    raise ValueError(f"Redis is not enabled in {backends_path}")


def _domain_paths(domain_path: Path, cache_plan_override: str | Path | None = None) -> tuple[Path, Path]:
    raw = json.loads(domain_path.read_text(encoding="utf-8"))
    paths = raw.get("paths", {})
    backends = (domain_path.parent / paths["backendConfig"]).resolve()
    if cache_plan_override is not None:
        cache_plan = Path(cache_plan_override).expanduser().resolve()
    else:
        cache_plan_value = paths.get("cachePlan", "cache/cache-plan.json")
        cache_plan = (domain_path.parent / cache_plan_value).resolve()
    return backends, cache_plan


def _iter_entries(index_path: Path, neighbors_path: Path) -> Iterator[tuple[int, bytes]]:
    with index_path.open("rb") as index, neighbors_path.open("rb") as neighbors:
        while record := index.read(INDEX_RECORD.size):
            if len(record) != INDEX_RECORD.size:
                raise ValueError(f"Truncated Redis cache index: {index_path}")
            source, offset, count = INDEX_RECORD.unpack(record)
            neighbors.seek(offset * UINT64.size)
            blob = neighbors.read(count * UINT64.size)
            if len(blob) != count * UINT64.size:
                raise ValueError(f"Truncated Redis adjacency file: {neighbors_path}")
            yield source, blob


def load(domain: str | Path, *, reset: bool, batch_size: int, cache_plan: str | Path | None = None) -> dict:
    try:
        import redis
    except ImportError as exc:
        raise RuntimeError("Redis loader requires `pip install -r tools/requirements.txt`") from exc

    domain_path = Path(domain).expanduser().resolve()
    backends_path, cache_plan_path = _domain_paths(domain_path, cache_plan)
    plan = json.loads(cache_plan_path.read_text(encoding="utf-8"))
    # The selected plan owns cache connection/materialization settings.  Fall
    # back to a legacy cache entry in backendConfig only for older plans.
    backend = next(
        (entry for entry in plan.get("runtimeBackends", []) if str(entry.get("id")) == "Redis"),
        None,
    )
    if backend is None:
        backend = _redis_backend(backends_path)
    connection = backend.get("connection", {})
    endpoint = str(connection.get("endpoint", "127.0.0.1:6379")).removeprefix("redis://")
    endpoint = endpoint.split("/", 1)[0]
    host, _, port = endpoint.rpartition(":")
    if not host:
        host, port = port, "6379"
    database = int(connection.get("database", 0) or 0)
    key_prefix = str(backend.get("key_prefix", "polygraph:adj:"))

    redis_manifest = Path(plan["artifacts"]["redis"]["manifest"])
    if not redis_manifest.is_absolute():
        redis_manifest = (cache_plan_path.parent / redis_manifest).resolve()
    manifest = json.loads(redis_manifest.read_text(encoding="utf-8"))
    index_path = redis_manifest.parent / manifest["index"]
    neighbors_path = redis_manifest.parent / manifest["neighbors"]

    client = redis.Redis(host=host, port=int(port), db=database, socket_timeout=30, decode_responses=False)
    client.ping()
    if reset:
        # Dedicated Redis instance/database is recommended for the graph cache.
        client.flushdb()
    # Never advertise a partially loaded cache as ready. The C++ runtime checks
    # this marker before selecting Redis for BFS expansion.
    client.delete(f"{key_prefix}__manifest")

    configured_limit = int(backend.get("memory_limit_bytes", 0) or 0)
    if configured_limit:
        client.config_set("maxmemory", configured_limit)
        client.config_set("maxmemory-policy", "noeviction")

    loaded = 0
    started = time.monotonic()
    last_report = started
    pipe = client.pipeline(transaction=False)
    for source, blob in _iter_entries(index_path, neighbors_path):
        pipe.set(f"{key_prefix}{source}", blob)
        loaded += 1
        if loaded % batch_size == 0:
            pipe.execute()
            pipe = client.pipeline(transaction=False)
        now = time.monotonic()
        if now - last_report >= 5.0:
            print(
                f"[redis-cache] loaded {loaded:,}/{int(manifest.get('vertices', 0)):,} vertices "
                f"({now - started:,.1f}s)",
                file=sys.stderr, flush=True,
            )
            last_report = now
    pipe.execute()
    readiness_manifest = {**manifest, "cachePlan": str(cache_plan_path), "loadedVertices": loaded}
    client.set(f"{key_prefix}__manifest", json.dumps(readiness_manifest, separators=(",", ":")).encode())

    return {
        "vertices": loaded,
        "relations": int(manifest.get("relations", 0)),
        "artifact_bytes": int(manifest.get("bytes", 0)),
        "endpoint": f"{host}:{port}",
        "database": database,
        "key_prefix": key_prefix,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Load a generated PolyGraph adjacency cache into Redis.")
    parser.add_argument("domain", help="Prepared domain.json")
    parser.add_argument("--cache-plan", help="Selected cache-plan.json; defaults to domain.paths.cachePlan")
    parser.add_argument("--no-reset", action="store_true", help="Keep unrelated keys; default is to flush the configured Redis DB")
    parser.add_argument("--batch-size", type=int, default=10_000)
    args = parser.parse_args()
    if args.batch_size <= 0:
        parser.error("--batch-size must be positive")
    print(json.dumps(load(args.domain, reset=not args.no_reset, batch_size=args.batch_size, cache_plan=args.cache_plan), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
