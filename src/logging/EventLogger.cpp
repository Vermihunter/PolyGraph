/**
 * @file src/logging/EventLogger.cpp
 * @brief Implementation for event logger in the event logging module.
 */

#include "logging/EventLogger.hpp"

#include "../../build/full-cpu/_deps/spdlog-src/include/spdlog/sinks/basic_file_sink.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace cpgdb {

    EventLogger::EventLogger(const std::filesystem::path &output_path) :
        start_time_(std::chrono::steady_clock::now()), output_(output_path, std::ios::out | std::ios::trunc) {
        if (!output_) {
            throw std::runtime_error("Failed to open event log file: " + output_path.string());
        }
    }

    void EventLogger::flush() {
        std::scoped_lock lock{mutex_};

        for (auto &sink: sinks_) {
            sink->flush();
        }
    }

    void EventLogger::add_sink(std::unique_ptr<IEventSink> sink) {
        std::scoped_lock lock{mutex_};
        sinks_.push_back(std::move(sink));
    }

    void EventLogger::event(EventLevel level, std::string_view event_name, json fields) {
        fields["seq"] = sequence_.fetch_add(1, std::memory_order_relaxed);
        fields["event"] = event_name;
        const auto [elapsed, clock_value] = get_times();
        fields["steady_ns"] = clock_value;
        fields["time"] = elapsed;
        fields["thread_id"] = thread_id();

        std::lock_guard lock(mutex_);
        // output_ << fields.dump() << '\n';

        for (auto &sink: sinks_) {
            sink->write(level, event_name, fields);
        }
    }

    std::pair<std::uint64_t, std::uint64_t> EventLogger::get_times() const {
        const auto now = std::chrono::steady_clock::now();
        return {static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time_).count()),
                static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count())};
    }

    std::uint64_t EventLogger::thread_id() {
        return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

} // namespace cpgdb
