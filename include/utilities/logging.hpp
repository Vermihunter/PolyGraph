/**
 * @file include/utilities/logging.hpp
 * @brief Declarations for logging in the utility module.
 */

#ifndef CPGDB_UTILITIES_LOGGING_HPP
#define CPGDB_UTILITIES_LOGGING_HPP

#include "logging/EventLevel.hpp"
#include "logging/EventLogger.hpp"
#include "logging/EventScope.hpp"

#include <memory>

namespace cpgdb {

using logger_t = std::unique_ptr<EventLogger>;
extern logger_t logger;
[[nodiscard]] EventLogger* event_logger() noexcept;

template <typename NameFactory, typename FieldsFactory>
[[nodiscard]] EventScope
make_event_scope(EventLevel level, NameFactory&& make_name, FieldsFactory&& make_fields) {

    auto* current_logger = event_logger();

    if (!current_logger) {
        return {};
    }

    return EventScope{
        current_logger,
        level,
        std::forward<NameFactory>(make_name)(),
        std::forward<FieldsFactory>(make_fields)(),
    };
}

} // namespace cpgdb

/*
#define EVENT(name, ...)                                                                           \
    do {                                                                                           \
        if (auto* cpgdb_event_logger_ = ::cpgdb::event_logger()) {                                 \
            cpgdb_event_logger_->event((name), nlohmann::json{__VA_ARGS__});                       \
        }                                                                                          \
    } while (false)

#define EVENT_SCOPE(var_name, name, ...)                                                           \
    ::cpgdb::EventScope var_name(::cpgdb::event_logger(), (name), nlohmann::json{__VA_ARGS__})
*/

#endif // CPGDB_UTILITIES_LOGGING_HPP
