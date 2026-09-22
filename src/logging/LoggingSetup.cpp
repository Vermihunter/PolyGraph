/**
 * @file src/logging/LoggingSetup.cpp
 * @brief spdlog-backed operational application logger setup.
 */
#include "logging/LoggingSetup.hpp"

#include "logging/Logger.hpp"
#include "observability/detail/PeriodicFlusher.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <stdexcept>
#include <utility>
#include <vector>

namespace polygraph::logging {
    namespace {

        std::shared_ptr<spdlog::details::thread_pool> log_thread_pool;
        observability::detail::PeriodicFlusher log_flusher;

        [[nodiscard]] spdlog::level::level_enum to_spdlog(const observability::LogLevel level) noexcept {
            using observability::LogLevel;
            switch (level) {
                case LogLevel::Trace:
                    return spdlog::level::trace;
                case LogLevel::Debug:
                    return spdlog::level::debug;
                case LogLevel::Info:
                    return spdlog::level::info;
                case LogLevel::Warn:
                    return spdlog::level::warn;
                case LogLevel::Error:
                    return spdlog::level::err;
                case LogLevel::Critical:
                    return spdlog::level::critical;
                case LogLevel::Off:
                    return spdlog::level::off;
            }
            return spdlog::level::off;
        }

        [[nodiscard]] spdlog::async_overflow_policy
        to_spdlog(const observability::AsyncOverflowPolicy policy) noexcept {
            return policy == observability::AsyncOverflowPolicy::Block ? spdlog::async_overflow_policy::block
                                                                       : spdlog::async_overflow_policy::overrun_oldest;
        }

        [[nodiscard]] std::shared_ptr<spdlog::sinks::sink> make_sink(const observability::LogSinkOptions &options) {
            using observability::SinkType;

            std::shared_ptr<spdlog::sinks::sink> sink;
            switch (options.type) {
                case SinkType::Stdout:
                    sink = options.color ? std::static_pointer_cast<spdlog::sinks::sink>(
                                                   std::make_shared<spdlog::sinks::stdout_color_sink_mt>())
                                         : std::static_pointer_cast<spdlog::sinks::sink>(
                                                   std::make_shared<spdlog::sinks::stdout_sink_mt>());
                    break;
                case SinkType::Stderr:
                    sink = options.color ? std::static_pointer_cast<spdlog::sinks::sink>(
                                                   std::make_shared<spdlog::sinks::stderr_color_sink_mt>())
                                         : std::static_pointer_cast<spdlog::sinks::sink>(
                                                   std::make_shared<spdlog::sinks::stderr_sink_mt>());
                    break;
                case SinkType::BasicFile:
                    if (!options.path.parent_path().empty()) {
                        std::filesystem::create_directories(options.path.parent_path());
                    }
                    sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(options.path.string(), options.truncate);
                    break;
                case SinkType::RotatingFile:
                    if (!options.path.parent_path().empty()) {
                        std::filesystem::create_directories(options.path.parent_path());
                    }
                    sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                            options.path.string(), options.max_size_bytes, options.max_files, options.rotate_on_open);
                    break;
            }

            sink->set_level(to_spdlog(options.minimum_level));
            sink->set_pattern(!options.pattern.empty() ? options.pattern : "%Y-%m-%dT%H:%M:%S.%e%z [%l] %v");
            return sink;
        }

    } // namespace

    void install_logger(const observability::LoggingOptions &options) {
        shutdown();
        if (!options.enabled) {
            return;
        }

        std::vector<spdlog::sink_ptr> sinks;
        observability::LogLevel flush_level = observability::LogLevel::Off;
        for (const auto &sink_options: options.sinks) {
            if (!sink_options.enabled) {
                continue;
            }
            sinks.push_back(make_sink(sink_options));
            if (flush_level == observability::LogLevel::Off || sink_options.flush_level < flush_level) {
                flush_level = sink_options.flush_level;
            }
        }
        if (sinks.empty()) {
            return;
        }

        std::shared_ptr<spdlog::logger> configured;
        if (options.asynchronous) {
            log_thread_pool =
                    std::make_shared<spdlog::details::thread_pool>(options.queue_size, options.worker_threads);
            configured = std::make_shared<spdlog::async_logger>(options.logger_name, sinks.begin(), sinks.end(),
                                                                log_thread_pool, to_spdlog(options.overflow_policy));
        } else {
            configured = std::make_shared<spdlog::logger>(options.logger_name, sinks.begin(), sinks.end());
        }

        configured->set_level(to_spdlog(options.minimum_level));
        if (flush_level != observability::LogLevel::Off) {
            configured->flush_on(to_spdlog(flush_level));
        }
        install(std::move(configured));
        log_flusher.start(options.periodic_flush_ms, [] {
            if (auto *current = logger()) {
                current->flush();
            }
        });
    }

    void shutdown() noexcept {
        try {
            log_flusher.stop();
            auto current = release();
            if (current) {
                current->flush();
            }
            current.reset();
            log_thread_pool.reset();
        } catch (...) {
            // Logging teardown must not mask application failures.
        }
    }

} // namespace polygraph::logging
