# Remote backend request scheduling

PolyGraph uses one server-lifetime `BackendRequestScheduler` per remote backend. The scheduler sits between BFS/query workers and Neo4j/ArangoDB. A BFS may request a high local concurrency, but the backend-global `max_in_flight` is never exceeded across all simultaneous HTTP requests.

## Configuration

Scheduler settings live on each backend entry:

```json
{
  "id": "ArangoDB",
  "kind": "ArangoDB",
  "scheduler": {
    "max_in_flight": 128,
    "max_queued": 8192,
    "max_queued_per_query": 1024,
    "policy": "priority-weighted-fair",
    "priority_aging_ms": 5000
  }
}
```

`max_in_flight` is a hard server-global limit. `max_queued` and `max_queued_per_query` provide producer backpressure instead of allowing an arbitrary number of pending DB operations to accumulate.

The existing `arangoMaxConcurrentRequests` and `neo4jMaxConcurrentRequests` HTTP fields are now per-BFS limits. For example, if three BFS requests each request 300 Arango operations but the Arango scheduler has `max_in_flight=128`, the total number of active Arango queries remains at or below 128. Each individual BFS is also limited by its own requested value.

## Policies

- `fifo`: oldest eligible DB operation first. This is useful for deterministic batch processing but a large BFS can dominate latency.
- `round-robin`: rotates across active BFS query IDs. This is the simplest starvation-resistant multi-tenant policy.
- `priority`: highest effective priority first. HTTP clients set `backendSchedulerPriority` in `[-100,100]`. Priority aging adds one priority point per `priority_aging_ms`, so low-priority work eventually catches up instead of starving forever.
- `weighted-fair`: smooth weighted sharing across active BFS requests. `backendSchedulerWeight` is in `[1,64]`; a request with weight 4 receives approximately four times the admissions of weight 1 while both remain backlogged.
- `priority-weighted-fair`: recommended server policy. PolyGraph first selects the highest effective priority class, including aging, then performs smooth weighted-fair admission among requests in that class.

Typical HTTP request hints:

```json
{
  "sourceVertexId": "...",
  "maxDepth": 4,
  "returnPredecessors": false,
  "arangoMaxConcurrentRequests": 300,
  "neo4jMaxConcurrentRequests": 100,
  "backendSchedulerPriority": 10,
  "backendSchedulerWeight": 2
}
```

The priority/weight values are hints inside the global policy; they never override backend-global safety limits.

## What occupies one slot

A scheduler slot represents one logical backend query. An Arango cursor keeps its slot from the initial POST through all cursor continuation requests. Retry backoff does not monopolize a slot: a failed attempt releases its lease before sleeping and reacquires admission for the next attempt.

## Observability

The scheduler emits structured events:

- `backend.scheduler.enqueue`
- `backend.scheduler.dequeue`
- `backend.scheduler.operation.start/end`
- `backend.scheduler.release`

Fields include backend name, query execution ID, priority, weight, queue wait, global in-flight count, queue size, global limit, and per-query limit. This allows benchmark analysis to separate database service time from time deliberately spent waiting behind admission control.
