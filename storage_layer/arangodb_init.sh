#!/bin/sh

set -e

echo "Creating people collection (if not exists)..."
# curl -u root:rootpassword -X POST http://localhost:8529/_api/collection \
#      -H "Content-Type: application/json" \
#      -d '{"name":"people"}' || true

# echo "Inserting people..."
# curl -u root:rootpassword \
#   -X POST "http://localhost:8529/_api/document/people" \
#   -H "Content-Type: application/json" \
#   -d '{"name":"Alice"}'

# curl -u root:rootpassword \
#   -X POST "http://localhost:8529/_api/document/people" \
#   -H "Content-Type: application/json" \
#   -d '{"name":"Bob"}'

# Collections
curl -u root:rootpassword -X POST http://localhost:8529/_api/collection \
  -H "Content-Type: application/json" \
  -d '{"name":"users"}' || true

curl -u root:rootpassword -X POST http://localhost:8529/_api/collection \
  -H "Content-Type: application/json" \
  -d '{"name":"orders"}' || true

curl -u root:rootpassword -X POST http://localhost:8529/_api/collection \
  -H "Content-Type: application/json" \
  -d '{"name":"payments"}' || true

# Edge collections
curl -u root:rootpassword -X POST http://localhost:8529/_api/collection \
  -H "Content-Type: application/json" \
  -d '{"name":"userPlacedOrder","type":3}' || true

curl -u root:rootpassword -X POST http://localhost:8529/_api/collection \
  -H "Content-Type: application/json" \
  -d '{"name":"orderPaidBy","type":3}' || true


# Users
curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/users" \
  -H "Content-Type: application/json" \
  -d '{"_key":"u1","name":"Alice","email":"alice@mail.com"}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/users" \
  -H "Content-Type: application/json" \
  -d '{"_key":"u2","name":"Bob","email":"bob@mail.com"}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/users" \
  -H "Content-Type: application/json" \
  -d '{"_key":"u3","name":"Charlie","email":"charlie@mail.com"}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/users" \
  -H "Content-Type: application/json" \
  -d '{"_key":"u4","name":"Diana","email":"diana@mail.com"}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/users" \
  -H "Content-Type: application/json" \
  -d '{"_key":"u5","name":"Eva","email":"eva@mail.com"}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/users" \
  -H "Content-Type: application/json" \
  -d '{"_key":"u6","name":"Frank","email":"frank@mail.com"}'

# Orders
curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/orders" \
  -H "Content-Type: application/json" \
  -d '{"_key":"o100","userKey":"u1","productIds":["p1","p2"],"total":120.5}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/orders" \
  -H "Content-Type: application/json" \
  -d '{"_key":"o101","userKey":"u1","productIds":["p3"],"total":89.0}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/orders" \
  -H "Content-Type: application/json" \
  -d '{"_key":"o102","userKey":"u2","productIds":["p2","p4"],"total":210.0}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/orders" \
  -H "Content-Type: application/json" \
  -d '{"_key":"o103","userKey":"u3","productIds":["p1","p5"],"total":310.0}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/orders" \
  -H "Content-Type: application/json" \
  -d '{"_key":"o104","userKey":"u4","productIds":["p6"],"total":49.0}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/orders" \
  -H "Content-Type: application/json" \
  -d '{"_key":"o105","userKey":"u5","productIds":["p7","p8"],"total":500.0}'

curl -u root:rootpassword \
  -X POST "http://localhost:8529/_api/document/orders" \
  -H "Content-Type: application/json" \
  -d '{"_key":"o106","userKey":"u6","productIds":["p3","p9"],"total":130.0}'

# Edges
# Alice -> o100
curl -u root:rootpassword \
  -X POST http://localhost:8529/_api/document/userPlacedOrder \
  -H "Content-Type: application/json" \
  -d '{"_from":"users/u1","_to":"orders/o100"}'

curl -u root:rootpassword \
  -X POST http://localhost:8529/_api/document/userPlacedOrder \
  -H "Content-Type: application/json" \
  -d '{"_from":"users/u1","_to":"orders/o101"}'

echo "ArangoDB init done."
