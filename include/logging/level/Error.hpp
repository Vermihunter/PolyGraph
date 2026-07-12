//
// Created by akos on 11. 7. 2026.
//

#ifndef CPGDB_ERROR_HPP
#define CPGDB_ERROR_HPP

#include "LoggingLevels.hpp"

#if CPGDB_EVENT_COMPILED_LEVEL <= CPGDB_EVENT_LEVEL_ERROR
#define EVENT_ERROR(name, ...) CPGDB_EVENT_IMPL(::cpgdb::EventLevel::Error, name, __VA_ARGS__)
#else
#define EVENT_ERROR(...) ((void)0)
#endif

#endif // CPGDB_ERROR_HPP
