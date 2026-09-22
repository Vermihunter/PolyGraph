# C++ API conventions

## IDs

- `GlobalId` is `std::uint64_t` (`include/data_model/EntityId.hpp`).
- High location bits encode authoritative ownership and, in legacy IDs, optional cache residency.
- New external cache plans use `canonical_id()` plus `residency.bin` instead of mutating IDs.
- `TypeId` identifies logical node/edge types; `BackendId` identifies physical stores.

## Ownership

- `std::unique_ptr` represents exclusive ownership.
- `std::shared_ptr` is used for stores and services shared through `DatabaseContext`/registries.
- References and spans are non-owning and must not outlive their backing object/storage.
- Batch APIs prefer `std::span<const NodeRef>` to avoid copying frontiers.

## Errors

- `std::invalid_argument`: malformed configuration/caller input.
- `std::out_of_range`: missing indexed value where that contract applies.
- `std::runtime_error`: backend/protocol/runtime invariant failure.

Frontends translate runtime errors to their protocol-specific response form.

## Naming and layout

Namespaces follow responsibility (`polygraph::cli`, `polygraph::insertion`, `polygraph::observability`, etc.). Types use PascalCase; functions/variables use snake_case. Runtime-polymorphic interfaces use the `I...` naming convention where established, such as `IGraphStore`.

## Performance conventions

Hot traversal APIs should avoid per-neighbor heap allocation where practical, batch backend requests, move large vectors, and keep canonical-ID conversion explicit at cache boundaries. Logging/event macros should not be used as substitutes for data structures or correctness checks.
