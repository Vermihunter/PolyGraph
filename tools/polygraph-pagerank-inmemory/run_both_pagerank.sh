#!/usr/bin/env bash
set -Eeuo pipefail

SF="${SF:-1.0}"
REPS="${REPS:-10}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p results

SF="$SF" REPS="$REPS" \
  "$SCRIPT_DIR/run_neo4j_pagerank.sh" |
  tee results/neo4j.csv

SF="$SF" REPS="$REPS" \
  "$SCRIPT_DIR/run_arango_pagerank.sh" |
  tee results/arango.csv
