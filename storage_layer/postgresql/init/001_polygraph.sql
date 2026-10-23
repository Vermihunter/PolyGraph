-- PolyGraph PostgreSQL physical graph schema.
--
-- IMPORTANT: secondary traversal indexes are intentionally NOT created here.
-- The benchmark baseline is index-free by default. Primary-key indexes remain
-- because IDs are the stable identity/upsert key used by the loader.
CREATE TABLE IF NOT EXISTS polygraph_nodes(
    id BIGINT PRIMARY KEY,
    type TEXT NOT NULL,
    source TEXT NOT NULL,
    is_proxy BOOLEAN NOT NULL DEFAULT FALSE,
    cache_residency INTEGER NOT NULL DEFAULT 0,
    properties JSONB NOT NULL DEFAULT '{}'::jsonb
);

CREATE TABLE IF NOT EXISTS polygraph_edges(
    id BIGINT PRIMARY KEY,
    edge_type TEXT NOT NULL,
    source TEXT NOT NULL,
    from_id BIGINT NOT NULL,
    from_type TEXT NOT NULL,
    to_id BIGINT NOT NULL,
    to_type TEXT NOT NULL,
    properties JSONB NOT NULL DEFAULT '{}'::jsonb
);
