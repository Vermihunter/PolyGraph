# Neo4J query

```sh
curl -s -u neo4j:password \
  -H 'Accept: application/json;charset=UTF-8' \
  -H 'Content-Type: application/json' \
  http://localhost:7474/db/neo4j/tx/commit \
  --data-raw '{
    "statements": [
      {
        "statement": "CALL { MATCH (n) WHERE NOT n:Proxy RETURN count(n) AS real_nodes, collect(n.uuid)[0..3] AS sample_real_nodes } CALL { MATCH (p:Proxy) RETURN count(p) AS proxy_nodes, collect(p.uuid)[0..3] AS sample_proxy_nodes } CALL { MATCH (a)-[r]->(b) RETURN count(r) AS relationships, collect({rel_uuid: r.uuid, from_uuid: a.uuid, to_uuid: b.uuid})[0..3] AS sample_relationships } RETURN real_nodes, proxy_nodes, relationships, sample_real_nodes, sample_proxy_nodes, sample_relationships"
      }
    ]
  }'
```

## Neighbors

```sh
curl -s -u neo4j:password \
  -H 'Accept: application/json;charset=UTF-8' \
  -H 'Content-Type: application/json' \
  http://localhost:7474/db/neo4j/tx/commit \
  --data-raw '{
    "statements": [
      {
        "statement": "MATCH (n)-[]->(neighbor) WHERE n.uuid = $globalNodeId RETURN { id: elementId(neighbor), labels: labels(neighbor), properties: properties(neighbor)} AS node;",
        "parameters": {"globalNodeId": "047fbced-eed8-430b-8c70-e1ed54c96030"}
      }
    ]
  }'
```

## Node with at least 3 relations

```sh
curl -s -u neo4j:password \
  -H 'Accept: application/json;charset=UTF-8' \
  -H 'Content-Type: application/json' \
  http://localhost:7474/db/neo4j/tx/commit \
  --data-raw '{
    "statements": [
      {
        "statement": "MATCH (n) WHERE COUNT { (n)--() } >= 3 RETURN n.uuid AS uuid LIMIT 5"
      }
    ]
  }'
```

result: 047fbced-eed8-430b-8c70-e1ed54c96030

## Vertices by uuid

```sh
curl -s -u neo4j:password \
  -H 'Accept: application/json;charset=UTF-8' \
  -H 'Content-Type: application/json' \
  http://localhost:7474/db/neo4j/tx/commit \
  --data-raw '{
    "statements": [
      {
        "statement": "MATCH (n) WHERE n.uuid = $globalNodeId RETURN { id: elementId(n), labels: labels(n), properties: properties(n)} AS node;",
        "parameters": {"globalNodeId": "047fbced-eed8-430b-8c70-e1ed54c96030"}
      }
    ]
  }'
```

## BFS

```shell
curl -u neo4j:password   -X POST "http://localhost:7474/db/neo4j/tx/commit"   -H "Content-Type: application/json"   -H "Accept: application/json"   -d '{
    "statements": [
      {
        "statement": "MATCH (source {uuid: $sourceUuid}) CALL gds.bfs.stream($graphName, {sourceNode: source, maxDepth: toInteger(toString($maxDepth))}) YIELD sourceNode, nodeIds, path RETURN sourceNode, nodeIds, path",
        "parameters": {
          "graphName": "ldbc-snb-sf0_1-hash-based",
          "sourceUuid": "d6e584ea-bee0-562c-bd82-0af0a658ed38",
          "maxDepth": 30
        }
      }
    ]
  }'
```

# ArangoDB query

```sh
curl -s -u root:rootpassword \
  'http://localhost:8529/_db/university/_api/collection?excludeSystem=true' \
| jq -r '.result[] | "\(.name)\t\(.type)"' \
| while IFS=$'\t' read -r name type; do
    count=$(curl -s -u root:rootpassword \
      "http://localhost:8529/_db/university/_api/collection/$name/count" \
      | jq -r '.count')

    case "$type:$name" in
      2:Proxy) kind="proxy_nodes" ;;
      2:*)     kind="real_nodes" ;;
      3:*)     kind="relationships" ;;
      *)       kind="other" ;;
    esac

    [ "$kind" != "other" ] && printf '%s\t%s\t%s\n' "$kind" "$name" "$count"
  done
```

```sh
curl -u root:rootpassword -X POST "http://localhost:8529/_db/university/_api/cursor"   -H "Content-Type: application/json"   -d '{
    "query": "FOR start IN @@collection FILTER start.uuid == @uuid FOR neighbor IN 1..1 ANY start @@edges1, @@edges2 RETURN neighbor",
    "bindVars": {n": "Assignment",
      "@collection": "Assignment",
      "@edges1": "HAS_ASSIGNMENT",
      "@edges2": "SUBMISSION_FOR",ad01-9337bcaa2732"
      "uuid": "4b83cee7-02df-48c9-ad01-9337bcaa2732"
    }
  }

```
