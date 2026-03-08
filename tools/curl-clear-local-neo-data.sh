curl -u neo4j:password \
    -H "Content-Type: application/json" \
    -X POST http://localhost:7474/db/neo4j/tx/commit \
    -d '{
    "statements": [
        {
        "statement": "MATCH (n) DETACH DELETE n"
        }
    ]
    }'