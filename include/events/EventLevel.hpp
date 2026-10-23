//
// Created by akos on 11. 7. 2026.
//

#ifndef POLYGRAPH_EVENTLEVEL_HPP
#define POLYGRAPH_EVENTLEVEL_HPP
#include <cstdint>

namespace polygraph {
enum class EventLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};
}

#endif // POLYGRAPH_EVENTLEVEL_HPP
