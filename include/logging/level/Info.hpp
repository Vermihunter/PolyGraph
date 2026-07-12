//
// Created by akos on 11. 7. 2026.
//

#ifndef CPGDB_INFO_HPP
#define CPGDB_INFO_HPP

#include "LoggingLevels.hpp"

#if CPGDB_EVENT_COMPILED_LEVEL <= CPGDB_EVENT_LEVEL_INFO

#define EVENT_INFO(name, ...) CPGDB_EVENT_IMPL(::cpgdb::EventLevel::Info, name, __VA_ARGS__)

#define EVENT_SCOPE_INFO(var_name, name, ...)                                                      \
    CPGDB_EVENT_SCOPE_IMPL(var_name, ::cpgdb::EventLevel::Info, name, __VA_ARGS__)

#define EVENT_SCOPE_INFO_FIELD(var_name, key, value)                                               \
    CPGDB_EVENT_SCOPE_FIELD_IMPL(var_name, key, value)

#else

#define EVENT_INFO(...) ((void)0)
#define EVENT_SCOPE_INFO(...) ((void)0)
#define EVENT_SCOPE_INFO_FIELD(...) ((void)0)

#endif

#endif // CPGDB_INFO_HPP
