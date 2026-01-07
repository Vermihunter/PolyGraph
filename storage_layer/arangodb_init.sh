#!/bin/sh

set -e

echo "Creating people collection (if not exists)..."
curl -u root:rootpassword -X POST http://localhost:8529/_api/collection \
     -H "Content-Type: application/json" \
     -d '{"name":"people"}' || true

echo "Inserting people..."
curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/people" \
  -H "Content-Type: application/json" \
  -d '{"name":"Alice"}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/people" \
  -H "Content-Type: application/json" \
  -d '{"name":"Bob"}'



echo "ArangoDB init done."
