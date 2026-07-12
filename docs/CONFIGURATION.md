# Runtime configuration

The BFS runtime consumes three JSON inputs:

1. a logical graph schema;
2. graph data used to construct host/GPU projection inputs;
3. an optional backend configuration.

Working templates are provided under `config/`.

## Logical graph schema

`nodeTypes` entries define a logical name, physical source backend, and property
schema. `relations` entries define the logical edge name, source backend,
endpoint node types, and properties.

Supported property types are `string`, `integer`, `boolean`, `float`, `date`, and
`datetime`.

The `source` value must be one of the backend aliases recognized by
`cpgdb::config::backend_id_from_string`, such as `Neo4J`, `ArangoDB`, or `GPU`.

## Graph data

The `nodes` array requires `uuid`, `type`, and `source`. The `relationships` array
requires `uuid`, `type`, `source`, `from`, and `to`. Optional `properties` values
are converted to the CPGDB `Value` model.

All relationship endpoint UUIDs must appear in the node array. Records whose
`source` is `GPU` are additionally copied into GPU projection input records.

## Backend configuration

Each backend entry supports:

- `id`, `kind`, `enabled`, `name`, and `priority`;
- `connection.endpoint`, `database`, `transport`, `bolt_endpoint`, credentials,
  and replica endpoints;
- capabilities for batch neighbors, native BFS, property filters, GPU execution,
  and replica parallelism;
- coverage mode and freshness metadata.

Coverage modes:

- `schema-source`: derive covered logical types from schema `source` fields;
- `all`: mark all node and edge types covered;
- `explicit`: provide `node_types`, `edge_types`, and all-type flags;
- `empty` or `none`: advertise no coverage.

Do not commit real credentials. Copy the example and inject secrets through your
deployment mechanism.

## Example CLI invocation

```bash
./build/cpu-bfs/bin/bfs \
  --logical-graph-schema config/logical_graph_schema.example.json \
  --data config/graph_data.example.json \
  --backend-config config/backends.example.json \
  --start-uuid 11111111-1111-4111-8111-111111111111 \
  --start-type Person \
  --max-depth 4 \
  --direction directed
```

The configured database must contain matching logical data for database-backed
queries. The example graph-data file alone does not seed Neo4j or ArangoDB.
