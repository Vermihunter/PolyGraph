include_guard(GLOBAL)

# Compatibility shim retained for downstream includes. PolyGraph no longer depends
# on Boost: logical entity identifiers are plain std::uint64_t values.
message(STATUS "PolyGraph dependency: Boost not required")
