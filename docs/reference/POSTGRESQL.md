# PostgreSQL backend

PolyGraph can use PostgreSQL as an authoritative persistent graph backend. The implementation is intentionally relational rather than emulating Cypher: vertices and relationships are stored in compact tables and large BFS frontiers are expanded with a set join.

## Dependencies

The C++ runtime uses PostgreSQL's official `libpq` client library. On Ubuntu:

```bash
sudo apt update
sudo apt install libpq-dev
```

The Python bulk loader uses psycopg 3 and is installed with the normal tools requirements:

```bash
python3 -m pip install -r tools/requirements.txt
```

## Physical schema

The Docker initialization file is `storage_layer/postgresql/init/001_polygraph.sql`.

```text
polygraph_nodes
  id BIGINT PRIMARY KEY
  type TEXT
  source TEXT
  is_proxy BOOLEAN
  cache_residency INTEGER
  properties JSONB

polygraph_edges
  id BIGINT PRIMARY KEY
  edge_type TEXT
  source TEXT
  from_id BIGINT
  from_type TEXT
  to_id BIGINT
  to_type TEXT
  properties JSONB
```

Traversal indexes are covering B-trees in both directions:

```sql
CREATE INDEX polygraph_edges_from_idx
ON polygraph_edges(from_id, edge_type) INCLUDE(to_id, to_type);

CREATE INDEX polygraph_edges_to_idx
ON polygraph_edges(to_id, edge_type) INCLUDE(from_id, from_type);
```

Endpoint types are stored on the edge row so a one-hop BFS expansion does not need to join `polygraph_nodes` merely to construct a `NodeRef`.

## Large-frontier BFS design

The PostgreSQL backend does not serialize a multi-million-vertex frontier into a SQL `IN (...)` clause. For each backend expansion it:

1. starts a transaction;
2. creates a session-local temporary frontier table;
3. sends the frontier once with binary `COPY`;
4. builds a temporary B-tree and runs `ANALYZE`;
5. joins `polygraph_frontier` with the indexed edge table;
6. enables libpq single-row mode so rows can be consumed without materializing the complete result;
7. for `--no-predecessors`, emits bounded candidate batches directly into PolyGraph's streaming BFS consumer;
8. commits, which drops the temporary frontier.

Outbound expansion is equivalent to:

```sql
SELECT e.to_id, e.to_type
FROM polygraph_frontier AS f
JOIN polygraph_edges AS e ON e.from_id = f.id;
```

Inbound expansion joins on `e.to_id`. `--direction both` combines both scans. Logical edge-type and target-node-type filters are pushed into SQL.

`server_side_candidate_deduplication` can be enabled in the backend JSON to wrap the expansion in `SELECT DISTINCT`. It is disabled by default because the optimal choice depends on result cardinality, PostgreSQL work memory, and how much duplication the global BFS visited set removes.

## Start PostgreSQL

The repository storage Compose stack contains a PostgreSQL service:

```bash
python3 tools/polygraph.py instance sf10_0 schema-based start
```

or use the repository's normal storage-layer Compose workflow. Defaults are in `storage_layer/.env` and can be overridden by a named instance environment.

Direct inspection:

```bash
docker compose --env-file storage_layer/.env -f storage_layer/docker-compose.yml exec postgresql \
  psql -U polygraph -d polygraph
```

Inside `psql`:

```sql
SELECT count(*) FROM polygraph_nodes;
SELECT count(*) FROM polygraph_edges;
\d+ polygraph_edges
```

## Backend configurations

Two ready-to-edit examples are provided:

```text
config/backends/backends.postgresql.json
config/backends/backends.neo-postgresql-gpu.json
```

A PostgreSQL backend entry has this form:

```json
{
  "id": "PostgreSQL",
  "kind": "PostgreSQL",
  "enabled": true,
  "connection": {
    "endpoint": "127.0.0.1:5432",
    "database": "polygraph",
    "credentials": {
      "username": "polygraph",
      "password": "polygraph"
    }
  },
  "server_side_candidate_deduplication": false
}
```

## Prepare an LDBC graph with PostgreSQL ownership

`tools/ldbc_snb/placement.postgresql.json` is an example placement that makes PostgreSQL the default owner while keeping selected node types on Neo4j.

For SF10, for example:

```bash
python3 tools/polygraph.py prepare ldbc \
  --input data/ldbc-snb/sf10_0/raw/social_network-sf10-CsvBasic-LongDateFormatter \
  --sf 10.0 \
  --domain-id ldbc-snb-sf10_0-postgresql \
  --output-dir data/ldbc-snb/sf10_0/postgresql \
  --placement-config tools/ldbc_snb/placement.postgresql.json \
  --storage-backends Neo4J PostgreSQL GPU \
  --root-count 100
```

The graph's `idRouting` metadata must include PostgreSQL before loading. Existing graphs whose IDs were generated for only Neo4j/ArangoDB/GPU cannot simply be pointed at a different backend config: regenerate/reconvert the graph with the desired placement so ownership bits and physical replication agree.

## Load

Load all authoritative stores:

```bash
python3 tools/polygraph.py load \
  data/ldbc-snb/sf10_0/postgresql/domain.json \
  --reset \
  --batch-size 20000
```

Only PostgreSQL:

```bash
python3 tools/polygraph.py load \
  data/ldbc-snb/sf10_0/postgresql/domain.json \
  --reset \
  --only-source PostgreSQL \
  --batch-size 20000
```

The Python loader uses psycopg `COPY`, creates proxy vertices required by cross-backend relationships, replicates relationships at endpoint owners according to the existing PolyGraph storage rule, and builds the traversal indexes after the bulk load.

## GlobalId location bit

PostgreSQL is backend ID `7` and uses location bit 15 in the fixed 16-bit location mask. GPU cache/owner bits therefore use bits 5 through 14 (`GPU:0` through `GPU:9`). Existing normal single-GPU datasets remain compatible. A dataset/artifact that actually encoded `GPU:10` must be regenerated before using this version because bit 15 is now PostgreSQL ownership.

PostgreSQL `BIGINT` is signed. PolyGraph preserves the complete unsigned `GlobalId` bit pattern by storing it as the equivalent two's-complement signed 64-bit value and bit-casting it back at the runtime boundary.

## Runtime events

The backend emits events such as:

```text
postgresql.stream_neighbor_candidates
postgresql.frontier.copy
postgresql.frontier.index
```

These make it possible to separate frontier transfer/index construction from the edge join when comparing PostgreSQL with ArangoDB, Neo4j, Redis, or GPU execution.
