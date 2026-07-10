from __future__ import annotations

import json
from pathlib import Path
from typing import Iterable

from .config import BenchmarkConfig


def write_backend_config(config: BenchmarkConfig, output: str | Path, enabled: Iterable[str]) -> Path:
    """Write a minimal backend config compatible with BackendConfigParser.hpp."""
    enabled_set = set(enabled)
    entries = [
        {
            "id": "Neo4J",
            "kind": "Neo4j",
            "enabled": "Neo4J" in enabled_set,
            "name": "Neo4j Primary",
            "priority": 50,
            "connection": {
                "endpoint": config.neo4j.http_endpoint,
                "database": "neo4j",
                "credentials": {"username": config.neo4j.user, "password": config.neo4j.password},
            },
            "capabilities": {"batch_neighbors": True, "native_bfs": False, "property_filters": True},
            "coverage": {"mode": "schema-source", "fresh": True, "complete_for_requested_subgraph": False},
        },
        {
            "id": "ArangoDB",
            "kind": "ArangoDB",
            "enabled": "ArangoDB" in enabled_set,
            "name": "ArangoDB Primary",
            "priority": 0,
            "connection": {
                "endpoint": f"{config.arango.base_url.rstrip('/')}/_db/{config.arango.database}/_api/cursor",
                "database": config.arango.database,
                "credentials": {"username": config.arango.user, "password": config.arango.password},
            },
            "capabilities": {"batch_neighbors": True, "native_bfs": False, "gpu_execution": False},
            "coverage": {"mode": "schema-source", "fresh": True, "complete_for_requested_subgraph": False},
        },
        {
            "id": "GPU",
            "kind": "CuGraph",
            "enabled": "GPU" in enabled_set,
            "name": "GPU acceleration",
            "priority": 1,
            "connection": {"endpoint": "", "database": ""},
            "capabilities": {"batch_neighbors": True, "native_bfs": False, "property_filters": False, "gpu_execution": True},
            "coverage": {"mode": "schema-source", "fresh": True, "complete_for_requested_subgraph": False},
        },
    ]
    out = Path(output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"version": 1, "backends": entries}, f, indent=2)
    return out
