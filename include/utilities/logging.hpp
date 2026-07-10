//
// Created by akos on 9. 7. 2026.
//

#ifndef CPGDB_LOGGING_HPP
#define CPGDB_LOGGING_HPP

#include <memory>

#include "logging/EventLogger.hpp"
#include "logging/EventScope.hpp"

using logger_t = std::unique_ptr<EventLogger>;

inline logger_t logger;


#define EVENT(name, ...) \
    (*logger).event((name), nlohmann::json{__VA_ARGS__})

#define EVENT_SCOPE(var_name, name, ...) \
    EventScope var_name((*logger), (name), nlohmann::json{__VA_ARGS__})


#endif //CPGDB_LOGGING_HPP
