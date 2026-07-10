//
// Created by akos on 9. 7. 2026.
//

#ifndef CPGDB_EVENTLOGGER_HPP
#define CPGDB_EVENTLOGGER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

class EventLogger {
public:
    using json = nlohmann::json;

    EventLogger(const std::filesystem::path& output_path)
        : start_time_(std::chrono::steady_clock::now()),
          out_(output_path, std::ios::out | std::ios::trunc) {
        if (!out_) {
            throw std::runtime_error("Failed to open event log file: " + output_path.string());
        }
    }

    void event(std::string_view event_name, json fields = json::object()) {
        const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed);

        json j = std::move(fields);

        j["seq"] = seq;
        j["event"] = std::string(event_name);

        auto [now, steady] = get_times();
        j["steady_ns"] = steady;
        j["time"] = now;
        j["thread_id"] = thread_id();

        {
            std::lock_guard<std::mutex> lock(mu_);
            out_ << j.dump() << '\n';
        }
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mu_);
        out_.flush();
    }

private:
    std::pair<uint64_t, uint64_t> get_times() const {
        const auto now = std::chrono::steady_clock::now();
        return std::pair {
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time_).count() ),
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count())
        };
    }

    static uint64_t thread_id() {
        return static_cast<uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id())
        );
    }

private:
    std::chrono::steady_clock::time_point start_time_;

    std::atomic<uint64_t> seq_{1};

    std::mutex mu_;
    std::ofstream out_;
};

#endif //CPGDB_EVENTLOGGER_HPP
