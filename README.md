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

## Arango queries

**Get node by elementId**

```sh
curl -X POST "http://localhost:8529/_api/cursor"   -u "root:rootpassword"   -H "Content-Type: application/json"   -d '{
    "query": "FOR v IN vertices FILTER v.elementId == @elementId LIMIT 1 RETURN v",
    "bindVars": {
      "elementId": ""
    }
  }'
```

**Get all neighbors of a concrete node identified by \_id**
