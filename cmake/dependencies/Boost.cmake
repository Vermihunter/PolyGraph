include_guard(GLOBAL)

# Compatibility shim retained for downstream includes. CPGDB no longer depends
# on Boost: logical entity identifiers are plain std::uint64_t values.
message(STATUS "CPGDB dependency: Boost not required")
