/**
 * @file src/events/EventScope.cpp
 * @brief RAII execution-event span implementation.
 */
#include "events/EventScope.hpp"

#include <utility>

namespace polygraph::events {

    EventScope::EventScope(EventCollector *collector, EventLevel level, std::string name, json fields) :
        collector_(collector), level_(level), name_(std::move(name)), fields_(std::move(fields)),
        start_(std::chrono::steady_clock::now()), active_(collector != nullptr) {
        if (collector_ != nullptr) {
            collector_->emit(level_, name_ + ".start", fields_);
        }
    }

    EventScope::EventScope(EventScope &&other) noexcept :
        collector_(other.collector_), level_(other.level_), name_(std::move(other.name_)),
        fields_(std::move(other.fields_)), end_fields_(std::move(other.end_fields_)), start_(other.start_),
        active_(other.active_) {
        other.active_ = false;
        other.collector_ = nullptr;
    }

    EventScope::~EventScope() noexcept {
        if (!active_ || collector_ == nullptr) {
            return;
        }

        try {
            json result = std::move(fields_);
            for (auto &[key, value]: end_fields_.items()) {
                result[key] = std::move(value);
            }
            result["duration_ms"] =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
            collector_->emit(level_, name_ + ".end", std::move(result));
        } catch (...) {
            // Instrumentation must never change application control flow.
        }
    }

    void EventScope::add_end_field(std::string key, json value) { end_fields_[std::move(key)] = std::move(value); }

} // namespace polygraph::events
