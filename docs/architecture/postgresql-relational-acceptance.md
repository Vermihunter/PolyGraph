# Relational PostgreSQL acceptance (#22)

The [reproducible example and run instructions](../examples/relational-postgresql/README.md)
combine the previously delivered components into two real workflows. The runner
is [relational_live.py](../../tools/ingestion/relational_live.py); its C++ probe is
[postgres_relational_acceptance_test.cpp](../../tests/postgres_relational_acceptance_test.cpp).
Expected BFS layers are literal fixture data, independent of the importer/readers.
Neither mocks nor skipped tests satisfy the three-service acceptance run.

## Requirements and evidence

| Issue | Acceptance criteria | Tests and contracts |
| --- | --- | --- |
| #13 | Node, junction and FK mappings; live catalog validation; clear invalid configurations | `postgres_relational_mapping_test`, `postgres_relational_config_test`, `integration.postgres-relational-metadata`; [mapping contract](postgresql-relational-mapping.md) |
| #14 | Typed table resolution/properties; different schemas/types; preserved DDL | `integration.postgres-relational-nodes`; acceptance property reader, exact native rows and before/after dumps |
| #15 | External keys; deterministic stable IDs; invalid/missing mapping; ingestion/traversal identity | `integration.postgres-relational-identity`, identity unit tests; acceptance distinct overlapping integer/text/UUID roots and owner checks; [generation lifecycle](postgresql-relational-identities.md) |
| #16 | Dedicated tables; endpoints; optional attributes; forward/reverse lookup | `integration.postgres-relational-relationships`; acceptance keyed parallel READS rows, native weights and rich/minimal/streamed directions |
| #17 | FK mappings; forward/reverse lookup; NULL; unchanged schema | `integration.postgres-relational-foreign-keys`; acceptance AT/MANAGER, NULL document/person FKs, self-loop/cycle and unchanged dumps |
| #18 | Typed native import; SQL types; identity catalog; bulk pipeline | `integration.postgres-relational-ingestion`; acceptance source-jsonl prepare/strict verify/execute/live verify and exact PK/property rows |
| #19 | Junction import; external endpoints; attributes; partitions/proxies | Same ingestion suite; acceptance two READS edges, OPENS/RETURNS remote references, native weights, external partition cuts and real Neo4j/ArangoDB replicas |
| #20 | FK assignment; referenced keys; NULL; invalid references/cardinality | Same ingestion suite, including both owner directions, conflicting targets, nullable/nonnullable cycles and rollback; acceptance FK writes/NULL and rejected invalid native/missing remote references |
| #21 | Junction/FK adjacency; partitions/proxies; cross-backend BFS; interface compatibility | `integration.postgres-relational-bfs` (PostgreSQL plus peer stand-ins); `integration.postgres-relational` adds actual three-service ownership handoff, all adjacency interfaces, predecessors and streamed CLI paths |
| #22 | Realistic normalized schemas; all keys/relationships/import/BFS; cross-backend and invalid cases | `integration.postgres-relational`: read-only populated attachment and empty native import, five types across app/geo/refs, eight vertices, eight logical edges, twelve hand-checked BFS cases per workflow, real PostgreSQL/Neo4j/ArangoDB |

Granular PostgreSQL suites use `POLYGRAPH_RELATIONAL_TEST_DSN` and skip when it is
unset. Supply a disposable PostgreSQL instance with CREATEDB/CREATEROLE for their
full coverage. The consolidated opt-in test owns its services and fails when
prerequisites are missing. It records image IDs, negative cases, workflow counts,
cleanup and preservation results in `integration-result.json`.

Generic PostgreSQL compatibility is protected by the relational BFS suite's
generic-store distance comparison and the existing
`integration.ingestion.polygraph-jsonl` three-service smoke. Full CPU tests remain
separate from explicitly enabled Docker acceptance tests.

## Limits retained for parent #12 review

This delivery covers the child issue acceptance criteria for scalar native
schemas. It does not broaden the documented initial policy: composite PKs and
unsupported SQL types/RLS are rejected; ownership/reference rows cannot mix in
one import table; remote-only FK targets need compatible local reference rows;
nonnullable FK type cycles are rejected. Generations require explicit refresh,
and runtime promises a per-chunk read snapshot rather than a whole-BFS snapshot.
Rich traversal does not assign canonical IDs to native edge PKs.

These limits are documented in the linked component contracts and example. Parent
#12 stays open until the operator explicitly accepts them or requests further
implementation. Child #22 completion does not merge or deploy the feature branch.
