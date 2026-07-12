//
// Created by akos on 11. 7. 2026.
//

#ifndef CPGDB_DEBUG_HPP
#define CPGDB_DEBUG_HPP

#include "LoggingLevels.hpp"

#if CPGDB_EVENT_COMPILED_LEVEL <= CPGDB_EVENT_LEVEL_DEBUG

#define EVENT_DEBUG(name, ...) CPGDB_EVENT_IMPL(::cpgdb::EventLevel::Debug, name, __VA_ARGS__)

#define EVENT_SCOPE_DEBUG(var_name, name, ...)                                                     \
    CPGDB_EVENT_SCOPE_IMPL(var_name, ::cpgdb::EventLevel::Debug, name, __VA_ARGS__)

#define EVENT_SCOPE_DEBUG_FIELD(var_name, key, value)                                              \
    CPGDB_EVENT_SCOPE_FIELD_IMPL(var_name, key, value)

#else

#define EVENT_DEBUG(...) ((void)0)
#define EVENT_SCOPE_DEBUG(...) ((void)0)
#define EVENT_SCOPE_DEBUG_FIELD(...) ((void)0)

#endif

#endif // CPGDB_DEBUG_HPP
