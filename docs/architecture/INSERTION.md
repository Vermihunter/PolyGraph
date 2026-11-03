# Graph IDs, preparation, and insertion

## Canonical IDs

PolyGraph uses uint64 global IDs. High bits encode the authoritative owner/routing location; the lower payload is stable backend-local identity. `canonical_global_id` removes cache-residency decoration while preserving the owner/payload identity.

New cache planning always indexes and emits canonical IDs. This also lets the planner consume an older decorated graph without propagating its old cache bits into a new plan.

## Preparation

Preparation converts source datasets into `graph.jsonl`, logical schema, roots, placement reports, backend configuration, and `domain.json`.

The recommended sequence is:

1. prepare the graph with authoritative placement;
2. load Neo4j/ArangoDB/PostgreSQL;
3. build cache plans separately.

Do not regenerate persistent graph data just to change Redis/GPU/MMAP policy.

## Loading

`tools/polygraph.py load DOMAIN` reads the prepared graph and dispatches records to configured persistent sources. The large-graph loader uses disk-backed endpoint metadata rather than retaining one Python object per graph vertex.

Progress batches report at approximately five-second intervals. Use `--batch-size` to tune request size and `--only-source` to isolate one backend.

## Native ID resolution

Neo4j loading may produce a resolved graph/native-ID map. The domain tooling records these sidecars so user-provided original start IDs can be mapped to the active uint64 IDs.

## Cache insertion is separate

Redis cache materialization is not part of persistent graph insertion. It consumes generated binary adjacency from a selected cache plan. GPU projection is built by the C++ runtime from the canonical graph plus selected residency.
