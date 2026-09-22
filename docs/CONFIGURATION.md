# Configuration

PolyGraph configuration is split by responsibility so graph data, storage ownership, cache strategy, and observability can evolve independently.

## Domain manifest

`domain.json` is the main runtime entry point. Its `paths` section references the logical schema, canonical graph, persistent backend config, observability config, optional native-ID map, and optionally a default cache plan.

A cache plan does not need to be written into the domain: `--cache-plan FILE` is the preferred experiment-time override.

## Backend configuration

The backend config contains:

- `idRouting`: ordered authoritative-owner layout encoded in IDs;
- persistent backend entries such as Neo4j, ArangoDB, and PostgreSQL;
- connection details and capabilities;
- priorities and coverage metadata.

In the new cache workflow, generated LDBC backend config is **authoritative-data-only**. GPU cache, Redis, and mmap execution entries can come from the selected cache plan's `runtimeBackends`.

## Cache configuration

Cache configuration JSON is an input to planning, not runtime graph identity. Example:

```json
{
  "version": 1,
  "priorities": {
    "GPU": 0,
    "MMap": 10,
    "Redis": 20,
    "Neo4J": 50,
    "ArangoDB": 50
  },
  "gpus": [
    {
      "device_id": 0,
      "memory": "4GiB",
      "relation_limit": 150000000,
      "regions": [
        {
          "name": "hot-vertices",
          "policy": {
            "kind": "top-degree",
            "vertex_count": 100000,
            "include_neighbors": true
          }
        }
      ]
    }
  ],
  "mmap": {"enabled": false},
  "redis": {
    "enabled": true,
    "endpoint": "127.0.0.1:6379",
    "memory": "2GiB",
    "vertex_limit": 1000000,
    "shared_hot_fraction": 0.075,
    "memory_utilization": 0.9,
    "entry_overhead_bytes": 128,
    "policy": {"kind": "top-degree"}
  }
}
```

Important Redis values:

- `shared_hot_fraction`: intentional GPU/Redis overlap; default `0.075` when omitted.
- `memory_utilization`: fraction of configured Redis memory made available to planned payload.
- `entry_overhead_bytes`: estimated per-key Redis overhead for budget planning.
- `vertex_limit`, `relation_limit`, `memory`: optional plan bounds.

## Cache plan output

`cache-plan.json` is generated. Do not hand-edit residency. It records region selection statistics, artifacts, runtime cache backends, the planning DB, and `residency.bin` metadata.

## Observability configuration

`config/observability/observability.json` independently controls:

- event gathering and JSONL sinks;
- operational logging and stdout/file sinks;
- metrics endpoint/namespace;
- process, allocator, GPU, and other collectors.

CLI paths override sinks without changing the config file. See [OBSERVABILITY.md](OBSERVABILITY.md).
