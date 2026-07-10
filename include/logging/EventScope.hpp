//
// Created by akos on 9. 7. 2026.
//

#ifndef CPGDB_EVENTSCOPE_HPP
#define CPGDB_EVENTSCOPE_HPP

#pragma once

#include "EventLogger.hpp"

class EventScope {
public:
    using json = nlohmann::json;

    EventScope(
        EventLogger& logger,
        std::string name,
        json fields = json::object()
    )
        : logger_(&logger),
          name_(std::move(name)),
          fields_(std::move(fields)),
          start_(std::chrono::steady_clock::now()) {
        logger_->event(name_ + ".start", fields_);
    }

    EventScope(const EventScope&) = delete;
    EventScope& operator=(const EventScope&) = delete;

    EventScope(EventScope&& other) noexcept
        : logger_(other.logger_),
          name_(std::move(other.name_)),
          fields_(std::move(other.fields_)),
          end_fields_(std::move(other.end_fields_)),
          start_(other.start_),
          active_(other.active_) {
        other.active_ = false;
    }

    ~EventScope() noexcept {
        if (!active_ || logger_ == nullptr) {
            return;
        }

        try {
            auto end = std::chrono::steady_clock::now();
            auto duration_us =
                std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();

            json j = fields_;

            for (auto& [key, value] : end_fields_.items()) {
                j[key] = value;
            }

            j["duration_us"] = duration_us;

            logger_->event(name_ + ".end", std::move(j));
        } catch (...) {
            // Never throw from destructor.
        }
    }

    void add_end_field(std::string key, json value) {
        end_fields_[std::move(key)] = std::move(value);
    }

private:
    EventLogger* logger_;
    std::string name_;

    json fields_;
    json end_fields_;

    std::chrono::steady_clock::time_point start_;
    bool active_ = true;
};

#endif //CPGDB_EVENTSCOPE_HPP
