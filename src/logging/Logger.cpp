#include "logging/Logger.hpp"

#include <atomic>
#include <utility>

namespace polygraph::logging {
    namespace {
        std::shared_ptr<spdlog::logger> logger_owner;
        std::atomic<spdlog::logger *> active_logger{nullptr};
    } // namespace

    spdlog::logger *logger() noexcept { return active_logger.load(std::memory_order_relaxed); }

    void install(std::shared_ptr<spdlog::logger> new_logger) noexcept {
        logger_owner = std::move(new_logger);
        active_logger.store(logger_owner.get(), std::memory_order_release);
    }

    std::shared_ptr<spdlog::logger> release() noexcept {
        active_logger.store(nullptr, std::memory_order_release);
        return std::move(logger_owner);
    }

} // namespace polygraph::logging
