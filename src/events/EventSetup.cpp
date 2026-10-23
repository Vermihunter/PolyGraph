/**
 * @file src/events/EventSetup.cpp
 * @brief Configured structured execution-event gathering setup.
 */
#include "events/EventSetup.hpp"

#include "events/EventAccess.hpp"
#include "events/sinks/JsonEventSerializer.hpp"
#include "events/sinks/SpdlogEventSink.hpp"
#include "events/sinks/TextEventSerializer.hpp"
#include "observability/detail/PeriodicFlusher.hpp"

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

namespace polygraph::events {
    namespace {

        std::shared_ptr<spdlog::details::thread_pool> event_thread_pool;
        observability::detail::PeriodicFlusher event_flusher;

        [[nodiscard]] std::unique_ptr<IEventSerializer> make_serializer(const observability::EventFormat format) {
            if (format == observability::EventFormat::Json) {
                return std::make_unique<JsonEventSerializer>();
            }
            return std::make_unique<TextEventSerializer>();
        }

        [[nodiscard]] spdlog::async_overflow_policy
        to_spdlog(const observability::AsyncOverflowPolicy policy) noexcept {
            return policy == observability::AsyncOverflowPolicy::Block ? spdlog::async_overflow_policy::block
                                                                       : spdlog::async_overflow_policy::overrun_oldest;
        }

        [[nodiscard]] std::shared_ptr<spdlog::sinks::sink> make_sink(const observability::EventSinkOptions &options) {
            using observability::SinkType;

            switch (options.type) {
                case SinkType::Stdout:
                    if (options.color)
                        return std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                    return std::make_shared<spdlog::sinks::stdout_sink_mt>();
                case SinkType::Stderr:
                    if (options.color)
                        return std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
                    return std::make_shared<spdlog::sinks::stderr_sink_mt>();
                case SinkType::BasicFile:
                    if (!options.path.parent_path().empty()) {
                        std::filesystem::create_directories(options.path.parent_path());
                    }
                    return std::make_shared<spdlog::sinks::basic_file_sink_mt>(options.path.string(), options.truncate);
                case SinkType::RotatingFile:
                    if (!options.path.parent_path().empty()) {
                        std::filesystem::create_directories(options.path.parent_path());
                    }
                    return std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                            options.path.string(), options.max_size_bytes, options.max_files, options.rotate_on_open);
            }
            throw std::logic_error("Unsupported event sink type");
        }

        [[nodiscard]] std::shared_ptr<spdlog::logger> make_transport(const observability::EventGatheringOptions &global,
                                                                     const observability::EventSinkOptions &options) {
            auto sink = make_sink(options);
            sink->set_pattern(!options.pattern.empty() ? options.pattern : "%v");

            const std::string logger_name = "polygraph.events." + options.name;
            if (!global.asynchronous) {
                return std::make_shared<spdlog::logger>(logger_name, std::move(sink));
            }

            return std::make_shared<spdlog::async_logger>(logger_name, std::move(sink), event_thread_pool,
                                                          to_spdlog(global.overflow_policy));
        }

    } // namespace

    std::unique_ptr<EventCollector> create_collector(const observability::EventGatheringOptions &options) {
#if !POLYGRAPH_ENABLE_EVENT_GATHERING
        (void) options;
        return nullptr;
#else
        if (!options.enabled) {
            return nullptr;
        }

        if (options.asynchronous) {
            event_thread_pool =
                    std::make_shared<spdlog::details::thread_pool>(options.queue_size, options.worker_threads);
        }

        std::vector<std::unique_ptr<IEventSink>> sinks;
        sinks.reserve(options.sinks.size());
        for (const auto &sink_options: options.sinks) {
            if (!sink_options.enabled) {
                continue;
            }
            sinks.push_back(std::make_unique<SpdlogEventSink>(
                    make_transport(options, sink_options), make_serializer(sink_options.format),
                    sink_options.minimum_level, sink_options.flush_level, sink_options.selection));
        }

        if (sinks.empty()) {
            event_thread_pool.reset();
            return nullptr;
        }

        return std::make_unique<EventCollector>(options.service, options.minimum_level, options.selection,
                                                std::move(sinks));
#endif
    }

    void install_collector(const observability::EventGatheringOptions &options) {
        shutdown();
        install(create_collector(options));
        if (collector() != nullptr) {
            event_flusher.start(options.periodic_flush_ms, [] {
                if (auto *current = collector()) {
                    current->flush();
                }
            });
        }
    }

    void shutdown() noexcept {
        try {
            event_flusher.stop();
            auto current = release();
            if (current) {
                current->flush();
            }
            current.reset();
            event_thread_pool.reset();
        } catch (...) {
            // Observability teardown must never mask the application's real failure.
        }
    }

} // namespace polygraph::events
