#!/bin/sh

set -e

#docker exec janusgraph sh -c "sed -i 's#channelizer: .*#channelizer: org.apache.tinkerpop.gremlin.server.channel.WsAndHttpChannelizer#' /opt/janusgraph/conf/janusgraph-server.yaml"

curl -X POST http://localhost:8182/gremlin \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"person\").property(\"name\",\"alice\")"}'

curl -X POST http://localhost:8182 \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.addV(\"person\").property(\"name\",\"bob\")"}'


curl -X POST http://localhost:8182 \
  -H "Content-Type: application/json" \
  -d '{"gremlin":"g.V().has(\"person\",\"name\",\"alice\").as(\"a\").V().has(\"person\",\"name\",\"bob\").addE(\"knows\").from(\"a\")"}'
