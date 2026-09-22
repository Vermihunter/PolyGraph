# Troubleshooting

## `Redis cache residency bit is set but the source key is absent`

New cache plans avoid this stale-bit failure in two ways:

1. external `residency.bin` supersedes cache bits embedded in old graph IDs;
2. Redis is eligible only when its readiness manifest exists and matches the selected cache plan.

Rebuild/materialize the selected plan:

```bash
python3 tools/polygraph.py cache build DOMAIN --config CONFIG.json
python3 tools/polygraph.py cache redis-load DOMAIN --cache-plan CACHE/cache-plan.json
```

Reset is the Redis loader default. If you used `--no-reset`, retry without it.

## Runtime appears stuck while loading GPU

Look for stdout or the selected `.log` file. Runtime startup now reports:

- schema/backend configuration load;
- cache-plan merge/residency load;
- GPU graph scan counts approximately every five seconds;
- selected vertex/edge totals;
- cuGraph CSR build heartbeat approximately every five seconds;
- projection ready.

A memory-limit failure means selected edges exceed the configured GPU planning/runtime budget. Reduce cache region size/relation limits or increase the configured device budget after confirming actual available GPU memory.

## `.jsonl` contains lines such as `[error] ...`

That is an operational text log saved under the wrong extension. Use:

```text
--log-file run.log
--event-file run.events.jsonl
```

The current CLI rejects `.jsonl`/`.ndjson` for `--log-file`.

## BFS CSV contains log lines

Use `--output-file result.csv`. Operational logs intentionally also go to stdout. `tools/polygraph.py run` does this automatically and keeps `events/`, `logs/`, and `outputs/` separate.

## Cache planning repeats graph indexing

The default DB is beside the canonical graph as `cache-plan.sqlite3`. Reuse happens only if its graph signature matches the graph path/size/mtime and the edge indexes are complete. A copied/modified graph or an older planning DB is deliberately rebuilt.

Override the shared path explicitly when needed:

```bash
python3 tools/polygraph.py cache build DOMAIN \
  --planning-db /fast-disk/cache-plan.sqlite3 \
  --config A.json B.json
```

## CMake cannot fetch dependencies

The normal presets use FetchContent. If DNS/network access is unavailable, configuration can fail before compilation while cloning a dependency. Provide dependencies locally or rerun in an environment with network access; this is separate from a source-code compile failure.
