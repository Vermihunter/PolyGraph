# Storage-layer inspection queries

## Neo4j

Real Neo4j entities use native `id(n)`/`id(r)` values and deliberately do not
store a `uuid` property. Only `:Proxy` nodes and replicated relationships keep a
numeric `uuid` property for a remote global ID.

### Counts and samples

```bash
curl -s -u neo4j:password \
  -H 'Content-Type: application/json' \
  http://localhost:7474/db/neo4j/tx/commit \
  --data-raw '{
    "statements": [{
      "statement": "CALL { MATCH (n:__GraphNode) WHERE NOT n:Proxy RETURN count(n) AS real_nodes, collect(id(n))[0..3] AS sample_native_node_ids } CALL { MATCH (p:Proxy) RETURN count(p) AS proxy_nodes, collect(p.uuid)[0..3] AS sample_proxy_global_ids } CALL { MATCH ()-[r]->() RETURN count(r) AS relationships, collect({native_id: id(r), replica_global_id: r.uuid})[0..3] AS sample_relationships } RETURN real_nodes, proxy_nodes, relationships, sample_native_node_ids, sample_proxy_global_ids, sample_relationships"
    }]
  }' | jq
```

### One native node and its neighbors

Pass the Neo4j-local payload, not the complete routed uint64 ID:

```bash
NATIVE_ID=123
curl -s -u neo4j:password \
  -H 'Content-Type: application/json' \
  http://localhost:7474/db/neo4j/tx/commit \
  --data-raw "{
    \"statements\": [{
      \"statement\": \"MATCH (n:__GraphNode) WHERE id(n) = \\$nativeId MATCH (n)-[r]-(neighbor) RETURN id(n) AS source_native_id, id(neighbor) AS neighbor_native_id, neighbor.uuid AS remote_global_id, labels(neighbor) AS labels, id(r) AS relationship_native_id, r.uuid AS replica_global_id\",
      \"parameters\": {\"nativeId\": ${NATIVE_ID}}
    }]
  }" | jq
```

### Create a GDS projection and run BFS

The benchmark code creates a projection named `UnifiedGraph` with every logical
relationship type projected as undirected. A simple all-types manual projection:

```bash
curl -s -u neo4j:password \
  -H 'Content-Type: application/json' \
  http://localhost:7474/db/neo4j/tx/commit \
  --data-raw '{
    "statements": [{
      "statement": "CALL gds.graph.drop($graphName, false) YIELD graphName RETURN graphName",
      "parameters": {"graphName": "UnifiedGraph"}
    }, {
      "statement": "CALL gds.graph.project($graphName, $nodeLabel, {ALL_RELATIONSHIPS: {type: \"*\", orientation: \"UNDIRECTED\"}}) YIELD graphName, nodeCount, relationshipCount RETURN graphName, nodeCount, relationshipCount",
      "parameters": {"graphName": "UnifiedGraph", "nodeLabel": "__GraphNode"}
    }]
  }' | jq
```

```bash
SOURCE_NATIVE_ID=123
curl -s -u neo4j:password \
  -H 'Content-Type: application/json' \
  http://localhost:7474/db/neo4j/tx/commit \
  --data-raw "{
    \"statements\": [{
      \"statement\": \"CALL gds.bfs.stream(\\$graphName, \\$configuration) YIELD sourceNode, nodeIds RETURN sourceNode, nodeIds\",
      \"parameters\": {
        \"graphName\": \"UnifiedGraph\",
        \"configuration\": {\"sourceNode\": ${SOURCE_NATIVE_ID}, \"maxDepth\": 7, \"logProgress\": false}
      }
    }]
  }" | jq
```

## ArangoDB

Every real/proxy vertex and every edge stores numeric `uuid`; `_key` is the
unsigned decimal global ID. Frontier expansion therefore uses direct
`DOCUMENT(collection, key)` point lookups rather than scanning the `uuid` index.

```bash
curl -s -u root:rootpassword \
  'http://localhost:8529/_db/university/_api/collection?excludeSystem=true' \
  | jq -r '.result[] | "\(.name)\t\(.type)"'
```

### Direct vertex lookup

```bash
curl -s -u root:rootpassword \
  -X POST 'http://localhost:8529/_db/university/_api/cursor' \
  -H 'Content-Type: application/json' \
  --data-raw '{
    "query": "RETURN DOCUMENT(@collection, @key)",
    "bindVars": {
      "collection": "Person",
      "key": "4611686018427388027"
    }
  }' | jq
```

### Batched frontier lookup pattern

```aql
FOR item IN @frontier
  LET start = DOCUMENT(item.collection, item.key)
  FILTER start != null
  FOR neighbor, edge IN 1..1 ANY start GRAPH @graphName
    RETURN { start: start.uuid, neighbor: neighbor.uuid, edge: edge.uuid }
```
