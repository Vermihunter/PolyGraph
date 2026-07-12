//
// Created by akos on 11. 7. 2026.
//

#ifndef CPGDB_EVENTLEVEL_HPP
#define CPGDB_EVENTLEVEL_HPP
#include <cstdint>

namespace cpgdb {
enum class EventLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};
}

#endif // CPGDB_EVENTLEVEL_HPP
