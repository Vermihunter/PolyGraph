/**
 * @file include/logging/Logger.hpp
 * @brief Operational application logging API.
 */
#pragma once

#include <memory>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <utility>

namespace polygraph::logging {

/** Non-owning hot-path pointer. Install/shutdown are application lifecycle operations. */
[[nodiscard]] spdlog::logger* logger() noexcept;
void install(std::shared_ptr<spdlog::logger> logger) noexcept;
std::shared_ptr<spdlog::logger> release() noexcept;

/**
 * Log a human/operational message. This API is intentionally independent from
 * EVENT_* instrumentation: it does not create timeline events or event JSONL.
 */
template <typename... Args>
void log(const spdlog::source_loc location,
         const spdlog::level::level_enum level,
         spdlog::format_string_t<Args...> format,
         Args&&... args) noexcept {
    try {
        auto* current = logger();
        if (current == nullptr || !current->should_log(level)) {
            return;
        }
        current->log(location, level, format, std::forward<Args>(args)...);
    } catch (...) {
        // Logging must not affect application control flow.
    }
}

} // namespace polygraph::logging
