#!/usr/bin/env bash
echo 'APOC is no longer required by v6. Neo4j BFS uses the already-installed GDS plugin.' >&2
echo 'Run: SF=1.0 ./prepare_neo4j_gds.sh' >&2
exit 0
