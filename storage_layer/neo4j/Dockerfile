ARG NEO4J_IMAGE_TAG=5
FROM neo4j:${NEO4J_IMAGE_TAG}

ENV NEO4J_server_http_enabled=true
ENV NEO4J_server_http_listen__address=0.0.0.0:7474
ENV NEO4J_server_bolt_listen__address=0.0.0.0:7687

ENV NEO4J_PLUGINS='["apoc"]'
ENV NEO4J_dbms_security_procedures_unrestricted=apoc.*
ENV NEO4J_dbms_security_procedures_allowlist=apoc.*

EXPOSE 7474 7687