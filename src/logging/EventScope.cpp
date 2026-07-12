/**
 * @file src/logging/EventScope.cpp
 * @brief Implementation for event scope in the event logging module.
 */

#include "logging/EventScope.hpp"

#include "logging/EventLevel.hpp"

#include <spdlog/spdlog.h>
#include <utility>

namespace cpgdb {

    EventScope::EventScope(EventLogger *logger, EventLevel level, std::string name, json fields) :
        logger_(logger), level_(level), name_(std::move(name)), fields_(std::move(fields)),
        start_(std::chrono::steady_clock::now()), active_(logger != nullptr) {

        if (logger_) {
            logger_->event(level_, name_ + ".start", fields_);
        }
    }

    EventScope::EventScope(EventLogger &logger, EventLevel level, std::string name, json fields) :
        EventScope(&logger, level, std::move(name), std::move(fields)) {}

    EventScope::EventScope(EventScope &&other) noexcept :
        logger_(other.logger_), level_(other.level_), name_(std::move(other.name_)), fields_(std::move(other.fields_)),
        end_fields_(std::move(other.end_fields_)), start_(other.start_), active_(other.active_) {

        other.active_ = false;
        other.logger_ = nullptr;
    }

    EventScope::~EventScope() noexcept {
        if (!active_ || !logger_) {
            return;
        }

        try {
            json result = std::move(fields_);

            for (auto &[key, value]: end_fields_.items()) {
                result[key] = std::move(value);
            }

            result["duration_ms"] =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_)
                            .count();

            logger_->event(level_, name_ + ".end", std::move(result));
        } catch (...) {
            // Destructors must not throw.
        }
    }

    void EventScope::add_end_field(std::string key, json value) { end_fields_[std::move(key)] = std::move(value); }

} // namespace cpgdb
