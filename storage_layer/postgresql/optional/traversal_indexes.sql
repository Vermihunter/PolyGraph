-- Optional PostgreSQL traversal indexes.
-- Normally create these through `tools/polygraph.py ... load --postgres-indexes`
-- so the benchmark manifest clearly records whether indexed loading was used.
CREATE INDEX IF NOT EXISTS polygraph_edges_from_idx
    ON polygraph_edges(from_id, edge_type) INCLUDE(to_id, to_type);
CREATE INDEX IF NOT EXISTS polygraph_edges_to_idx
    ON polygraph_edges(to_id, edge_type) INCLUDE(from_id, from_type);
CREATE INDEX IF NOT EXISTS polygraph_nodes_type_idx
    ON polygraph_nodes(type, id);
ANALYZE polygraph_nodes;
ANALYZE polygraph_edges;
