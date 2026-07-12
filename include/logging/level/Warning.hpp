//
// Created by akos on 11. 7. 2026.
//

#ifndef CPGDB_WARN_HPP
#define CPGDB_WARN_HPP

#include "LoggingLevels.hpp"

#if CPGDB_EVENT_COMPILED_LEVEL <= CPGDB_EVENT_LEVEL_WARN
#define EVENT_WARN(name, ...) CPGDB_EVENT_IMPL(::cpgdb::EventLevel::Warn, name, __VA_ARGS__)
#else
#define EVENT_WARN(...) ((void)0)
#endif

#endif // CPGDB_WARN_HPP
