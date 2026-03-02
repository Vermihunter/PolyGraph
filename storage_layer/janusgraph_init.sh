#!/bin/sh

set -e

# curl -X POST http://localhost:8182/gremlin \
#   -H "Content-Type: application/json" \
#   -d '{"gremlin":"g.addV(\"person\").property(\"name\",\"alice\")"}'

# curl -X POST http://localhost:8182 \
#   -H "Content-Type: application/json" \
#   -d '{"gremlin":"g.addV(\"person\").property(\"name\",\"bob\")"}'


# curl -X POST http://localhost:8182 \
#   -H "Content-Type: application/json" \
#   -d '{"gremlin":"g.V().has(\"person\",\"name\",\"alice\").as(\"a\").V().has(\"person\",\"name\",\"bob\").addE(\"knows\").from(\"a\")"}'

# Categories
curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"category\").property(\"name\",\"Electronics\")"}'

curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"category\").property(\"name\",\"Books\")"}'

curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"category\").property(\"name\",\"Home\")"}'


# Products
curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"product\").property(\"productId\",\"p1\").property(\"name\",\"Laptop\").property(\"price\",999)"}'

curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"product\").property(\"productId\",\"p2\").property(\"name\",\"Headphones\").property(\"price\",199)"}'

curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"product\").property(\"productId\",\"p3\").property(\"name\",\"Keyboard\").property(\"price\",120)"}'

curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"product\").property(\"productId\",\"p4\").property(\"name\",\"Monitor\").property(\"price\",300)"}'

curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"product\").property(\"productId\",\"p5\").property(\"name\",\"Tablet\").property(\"price\",450)"}'

curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"product\").property(\"productId\",\"p6\").property(\"name\",\"Book A\").property(\"price\",30)"}'

curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"product\").property(\"productId\",\"p7\").property(\"name\",\"Book B\").property(\"price\",25)"}'

curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"product\").property(\"productId\",\"p8\").property(\"name\",\"Chair\").property(\"price\",150)"}'

curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"product\").property(\"productId\",\"p9\").property(\"name\",\"Desk\").property(\"price\",400)"}'


# Edges
# Electronics
curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p1\").as(\"a\").V().has(\"category\",\"name\",\"Electronics\").addE(\"inCategory\").from(\"a\")"}'

curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p2\").as(\"a\").V().has(\"category\",\"name\",\"Electronics\").addE(\"inCategory\").from(\"a\")"}'

curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p3\").as(\"a\").V().has(\"category\",\"name\",\"Electronics\").addE(\"inCategory\").from(\"a\")"}'

curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p4\").as(\"a\").V().has(\"category\",\"name\",\"Electronics\").addE(\"inCategory\").from(\"a\")"}'

curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p5\").as(\"a\").V().has(\"category\",\"name\",\"Electronics\").addE(\"inCategory\").from(\"a\")"}'


# BOoks
curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p6\").as(\"a\").V().has(\"category\",\"name\",\"Books\").addE(\"inCategory\").from(\"a\")"}'

curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p7\").as(\"a\").V().has(\"category\",\"name\",\"Books\").addE(\"inCategory\").from(\"a\")"}'

# Home
curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p8\").as(\"a\").V().has(\"category\",\"name\",\"Home\").addE(\"inCategory\").from(\"a\")"}'

curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p9\").as(\"a\").V().has(\"category\",\"name\",\"Home\").addE(\"inCategory\").from(\"a\")"}'

# alsoBought
curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p3\").as(\"a\").V().has(\"product\",\"productId\",\"p4\").addE(\"alsoBought\").from(\"a\").property(\"weight\",0.65)"}'

curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p2\").as(\"a\").V().has(\"product\",\"productId\",\"p5\").addE(\"alsoBought\").from(\"a\").property(\"weight\",0.72)"}'

curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p5\").as(\"a\").V().has(\"product\",\"productId\",\"p1\").addE(\"alsoBought\").from(\"a\").property(\"weight\",0.55)"}'

curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p8\").as(\"a\").V().has(\"product\",\"productId\",\"p9\").addE(\"alsoBought\").from(\"a\").property(\"weight\",0.91)"}'



# coViewed
curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p4\").as(\"a\").V().has(\"product\",\"productId\",\"p5\").addE(\"coViewed\").from(\"a\").property(\"weight\",0.43)"}'

curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p6\").as(\"a\").V().has(\"product\",\"productId\",\"p7\").addE(\"coViewed\").from(\"a\").property(\"weight\",0.88)"}'

curl -X POST http://localhost:8182/gremlin \
-H "Content-Type: application/json" \
-d '{"gremlin":"g.V().has(\"product\",\"productId\",\"p1\").as(\"a\").V().has(\"product\",\"productId\",\"p4\").addE(\"coViewed\").from(\"a\").property(\"weight\",0.59)"}'