# Vermesa

Start ArangoDB and JanusGraph

```sh
cd storage_layer
docker compose up -d
./arangodb_init.sh
./janusgraph_init.sh
```

Execute process

```sh
cmake -B build -S .
cmake --build build
./src/core/graph_example ./components/data_access/JanusGraph/libjanus.so ./components/data_access/ArangoDB/libarango.so
```
