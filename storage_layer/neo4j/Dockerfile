ARG NEO4J_IMAGE_TAG=5
FROM neo4j:${NEO4J_IMAGE_TAG}

ENV NEO4J_server_http_enabled=true
ENV NEO4J_server_http_listen__address=0.0.0.0:7474
ENV NEO4J_server_bolt_listen__address=0.0.0.0:7687

# Native traversal benchmarks use Graph Data Science gds.bfs.stream.
ENV NEO4J_PLUGINS='["graph-data-science"]'

ENV NEO4J_dbms_security_procedures_unrestricted='gds.*'
ENV NEO4J_dbms_security_procedures_allowlist='gds.*'

EXPOSE 7474 7687