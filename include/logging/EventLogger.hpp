/**
 * @file include/logging/EventLogger.hpp
 * @brief Declarations for event logger in the event logging module.
 */

#ifndef CPGDB_LOGGING_EVENT_LOGGER_HPP
#define CPGDB_LOGGING_EVENT_LOGGER_HPP

#include "EventLevel.hpp"
#include "sinks/IEventSink.hpp"
#include "utilities/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string_view>

namespace cpgdb {

class EventLogger {
  public:
    using json = nlohmann::json;

    explicit EventLogger(const std::filesystem::path& output_path);
    void event(EventLevel level, std::string_view event_name, json fields = json::object());
    void add_sink(std::unique_ptr<IEventSink> sink);
    void flush();

  private:
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> get_times() const;
    [[nodiscard]] static std::uint64_t thread_id();

    std::chrono::steady_clock::time_point start_time_;
    std::vector<std::unique_ptr<IEventSink>> sinks_;
    std::atomic<std::uint64_t> sequence_{1};
    std::mutex mutex_;
    std::ofstream output_;
};

} // namespace cpgdb

#endif // CPGDB_LOGGING_EVENT_LOGGER_HPP
