# Issue #36 implementation plan

Parent: #26 (Phase 3), following #35. Extend existing ingestion and runtime paths; keep PostgreSQL's relational representation and BFS contracts intact.

1. Audit domain metadata, placement/proxies, caches, CSR builders and backend writers/query visitors.
2. Add unfamiliar mixed-schema fixtures (punctuation, Unicode, arbitrary properties, parallel edges, self-loop) and regressions for ingestion, response conversion, cache/CSR output and placement.
3. Reuse the existing ingestion naming convention at Neo4j/ArangoDB physical query boundaries. Preserve logical names in schema/proxy metadata. Fail on ambiguous mappings; enforce shared collection namespace only for ArangoDB.
4. Run focused tests, isolated three-backend import/BFS, full CPU/Python regressions, formatting and documentation checks. Report unavailable execution explicitly.
5. Review scope and compatibility; leave #36 available for review before committing.
