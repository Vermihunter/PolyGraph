# Isolated movie-graph live smoke test

Build the CPU importer example and BFS (`cmake --build --preset cpu-tests`). With Docker available and the three images already cached, run from the repository root:

```sh
python3 tools/ingestion/disposable_live.py --run --adapter movie-graph \
  --binary build/cpu-tests/bin/polygraph_movie_import \
  --bfs build/cpu-tests/bin/polygraph_bfs \
  --output-dir build/movie-live --unique-output
```

Defaults are `polygraph-neo4j:5.26-enterprise`, `polygraph-arangodb:3.12`, and `postgres:17`; override with `--neo-image`, `--arango-image`, and `--pg-image`. Missing binaries, the linked movie adapter, Docker, or cached images produce an `integration-result.json` with skip reasons and exit code 77. This movie path does not pull images automatically. `--run` is mandatory.

The command uses the existing generic `import --graph-manifest` workflow to prepare and execute the movie CSV fixture against newly created databases. Native live verification and independent reads of all three stores check node/edge IDs, logical and physical types, user attributes, proxies, replicas, and endpoint closure. The actual C++ BFS then checks all three hand-authored layer oracles in `examples/movie-graph/expected.json`, including directed traversal and two typed source roots. No embedded graph substitutes for backend reads.

Every run uses a fresh directory, generation-labelled containers, loopback ports, network, and bind-mounted data. Source configuration targets are replaced with the generated targets. Cleanup validates ownership before removing only the run's containers and network, including after failures. Pre-existing container and volume metadata is compared before and after; detected changes fail the test. Concurrent changes to those resources can therefore cause a conservative failure.

The result report includes prerequisites, stored counts, BFS results, preservation checks, duration, and cleanup status. Private logs, generated credentials, artifacts and bind-mounted data remain in the run directory for diagnosis; treat that directory as sensitive. The temporary runtime credential file is removed. Exit 0 requires all checks and cleanup to succeed; other failures return 1. If interrupted cleanup fails, inspect the report and use the existing ownership-checked `docker_load.py stop --state <run>/environment.json` recovery command.

CTest registration is opt-in through `POLYGRAPH_INGESTION_LIVE_TESTS=ON`; select `ctest --test-dir <build> -R '^integration.ingestion.movie-graph$' --output-on-failure`. Normal CPU CI only runs service-free assertion, opt-in, and failure-path tests.
