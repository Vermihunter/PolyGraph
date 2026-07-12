# CPGDB

CPGDB is a C++23 graph-processing system that presents one logical graph over multiple backends. It supports distributed breadth-first search across Neo4j and ArangoDB, optional CUDA/cuGraph acceleration, a command-line BFS client, and Drogon/gRPC service frontends.

## Requirements

- CMake 3.27 or newer
- Ninja
- A C++23 compiler
- Boost headers, libcurl, and `pkg-config`
- Docker with Docker Compose for the database backends
- Optional: CUDA, RAFT, RMM, and cuGraph for the `gpu-bfs` preset

On Ubuntu, the basic CPU dependencies can be installed with:

```bash
sudo apt update
sudo apt install build-essential ninja-build pkg-config     libboost-all-dev libcurl4-openssl-dev
```

Before the first build, review the machine-specific paths in `CMakePresets.json`, especially the Conda, CUDA, `clang-format`, and `clang-tidy` paths.

## Start the database backends

From the directory containing `compose.yaml` or `docker-compose.yml`:

```bash
docker compose up -d
docker compose ps
```

The provided configurations expect Neo4j on port `7474` and ArangoDB on port `8529`. Update the credentials and endpoints in `config/backends.*.json` when necessary.

To inspect or stop the containers:

```bash
docker compose logs -f
docker compose down
```

## Build

Configure and build the CPU BFS executable:

```bash
cmake --preset cpu-bfs
cmake --build --preset cpu-bfs
```

Configure and build the HTTP server:

```bash
cmake --preset http-server
cmake --build --preset http-server
```

For CUDA/cuGraph, use `gpu-bfs` instead of `cpu-bfs`.

## Run the BFS CLI

The BFS command requires a logical schema, graph data, and optionally a backend configuration:

```bash
./build/cpu-bfs/bin/bfs     --logical-graph-schema config/data/schema/university/sample-university-domain.graph-shape.json     --data config/data/full/university/large-university.json     --backend-config config/backends/backends.full.json     --start-uuid 11a96cb7-c043-4aa7-aa35-2297e9be7f06     --start-type Faculty     --max-depth 7     --direction undirected
```

Replace the source UUID with a vertex that exists in the loaded dataset. The result is printed as `vertex_id;distance` CSV.

See all available options with:

```bash
./build/cpu-bfs/bin/bfs --help
```

## Run the HTTP server

The checked-in HTTP configuration contains paths relative to the executable directory:

```bash
cd build/http-server/bin
./http_server --server-config ../../../config/drogon/http_server.full.json --log-file logs/double-log-test.jsonl
```

Check that the server is running:

```bash
curl http://localhost:8080/healthz
```

Run BFS through the HTTP API:

```bash
curl -X POST http://localhost:8080/api/v1/graphs/university/bfs     -H 'Content-Type: application/json'     -d '{
      "sourceVertexId": "11a96cb7-c043-4aa7-aa35-2297e9be7f06",
      "maxDepth": 3,
      "includeEdges": false
    }'
```

The server listens on `0.0.0.0:8080` by default. Backend connections, the logical schema, and the input data are configured in `config/http_server.full.json`.