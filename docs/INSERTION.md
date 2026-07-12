# Insertion subsystem {#insertion_architecture}

## Overview

The insertion subsystem separates four responsibilities:

1. **data ownership** — immutable `InsertionItem` values hold nodes or edges;
2. **placement policy** — an `IInsertionPlacementPolicy` chooses physical targets
   and an optional logical home backend;
3. **coordination** — `InsertionCoordinator` validates, groups, executes, and
   accounts for a write batch;
4. **backend execution** — query visitors translate `InsertQuery` and
   `InsertBatchQuery` to Neo4j Cypher or ArangoDB AQL.

## Type-based routing

`TypeBasedInsertionPlacementPolicy` has independent maps for node and edge type
IDs. Each route can contain one backend or multiple replicas. The first backend
is the logical home backend.

```cpp
using namespace cpgdb;
using namespace cpgdb::insertion;

TypeBasedInsertionPlacementPolicy::RouteMap node_routes{
    {person_type, {NEO4J}},
    {audit_type, {ARANGO}},
};
TypeBasedInsertionPlacementPolicy::RouteMap edge_routes{
    {knows_type, {NEO4J}},
    {emitted_type, {ARANGO}},
};

auto policy = std::make_shared<TypeBasedInsertionPlacementPolicy>(
    std::move(node_routes),
    std::move(edge_routes),
    std::vector<BackendId>{NEO4J},
    std::vector<BackendId>{ARANGO});
```

## Replicate everywhere

`ReplicateEverywhereInsertionPlacementPolicy` writes every entity to all selected
stores. `IHomeBackendSelector` independently chooses which backend owns the
entity. `LeastLoadedHomeBackendSelector` uses weighted stored/owned node/edge
counts and backend priority.

```cpp
auto selector = std::make_shared<LeastLoadedHomeBackendSelector>();
auto policy = std::make_shared<ReplicateEverywhereInsertionPlacementPolicy>(
    selector,
    std::vector<BackendId>{NEO4J, ARANGO});
```

An empty replica list means all registered stores. An explicit list is safer when
the registry contains caches, read-only stores, or GPU projections.

## UUID ownership encoding

`BackendEncodedUuid` stores the 32-bit home `BackendId` in UUID bytes 0–3 in
network byte order. Remaining bytes preserve random entropy and RFC 4122
version/variant bits.

```cpp
GlobalId id = BackendEncodedUuid::generate(NEO4J);
BackendId owner = BackendEncodedUuid::decode(id);
```

The encoded backend identifies logical ownership, not every replica location.

## Coordinator transaction flow

For each input entity, `InsertionCoordinator`:

1. obtains a placement decision;
2. validates target stores against the registry;
3. generates or rewrites the UUID when requested;
4. creates one immutable `InsertionItem`;
5. groups items by target backend;
6. submits one `InsertBatchQuery` per backend;
7. updates load accounting only after all backend calls succeed;
8. returns an `InsertionReceipt` for each entity.

## Load accounting

`BackendLoad` tracks:

- owned nodes;
- owned edges;
- physically stored nodes;
- physically stored edges.

`BackendLoadSnapshot` overlays reservations made during one planning batch so a
least-loaded selector does not repeatedly choose the same backend before durable
counters are committed.

## Advanced placement policies

Implement `IInsertionPlacementPolicy` for arbitrary logic. `PlacementContext`
provides the backend registry and a mutable load snapshot. Policies can consider:

- node/edge type;
- backend capabilities and kind;
- current and projected load;
- backend priority;
- tenant or geography affinity;
- endpoint locality for edges;
- capacity and cost weights;
- replication factor;
- maintenance state and health.

Implement `IHomeBackendSelector` when replica targets remain fixed but ownership
selection changes. Replace `IBackendLoadTracker` to use a durable catalog,
metrics service, or database-backed counters.

## Consistency limitations

The coordinator does not implement a cross-database transaction. A later backend
can fail after an earlier backend commits. Production deployments should make
writes idempotent and add an outbox, retry log, saga, or compensation process.
Edge endpoints must already exist on every backend selected for the edge.
