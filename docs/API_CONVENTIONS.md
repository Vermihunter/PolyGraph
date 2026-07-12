# API conventions

## Ownership

- `std::unique_ptr` means exclusive ownership.
- `std::shared_ptr` is used for stores, immutable insertion payloads, and runtime
  services shared across components.
- References represent non-owning dependencies that must outlive the receiver.
- Query results use `query_result_t`, an owning smart-pointer alias.

## Error handling

Functions throw `std::invalid_argument` for malformed caller input,
`std::out_of_range` for missing indexed values, and `std::runtime_error` for
backend/protocol failures or violated runtime invariants. Transport layers map
those failures to protocol-specific errors.

## IDs and graph types

- `GlobalId` is a Boost UUID.
- `TypeId` identifies logical node and edge types.
- `BackendId` identifies a physical backend.
- `GraphVersion` identifies the logical data version represented by coverage or
  a projection.

## Naming

Namespaces follow responsibility (`cpgdb::insertion`, `cpgdb::cli`,
`cpgdb::http`, and so on). Classes use PascalCase; functions and variables use
snake_case. Interfaces use an `I` prefix where runtime polymorphism is central.
