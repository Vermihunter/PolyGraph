//
// Created by akos on 11. 7. 2026.
//

#ifndef CPGDB_TRACE_HPP
#define CPGDB_TRACE_HPP

#include "LoggingLevels.hpp"

#if CPGDB_EVENT_COMPILED_LEVEL <= CPGDB_EVENT_LEVEL_TRACE

#define EVENT_TRACE(name, ...) CPGDB_EVENT_IMPL(::cpgdb::EventLevel::Trace, name, __VA_ARGS__)

#define EVENT_SCOPE_TRACE(var_name, name, ...)                                                     \
    CPGDB_EVENT_SCOPE_IMPL(var_name, ::cpgdb::EventLevel::Trace, name, __VA_ARGS__)

#define EVENT_SCOPE_TRACE_FIELD(var_name, key, value)                                              \
    CPGDB_EVENT_SCOPE_FIELD_IMPL(var_name, key, value)

#else

#define EVENT_TRACE(...) ((void)0)
#define EVENT_SCOPE_TRACE(...) ((void)0)
#define EVENT_SCOPE_TRACE_FIELD(...) ((void)0)

#endif

#endif // CPGDB_TRACE_HPP
