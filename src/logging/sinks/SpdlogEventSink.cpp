//
// Created by akos on 12. 7. 2026.
//

#include "logging/sinks/SpdlogEventSink.hpp"

namespace cpgdb {
    namespace {

        spdlog::level::level_enum to_spdlog_level(EventLevel level) {
            switch (level) {
                case EventLevel::Trace:
                    return spdlog::level::trace;
                case EventLevel::Debug:
                    return spdlog::level::debug;
                case EventLevel::Info:
                    return spdlog::level::info;
                case EventLevel::Warn:
                    return spdlog::level::warn;
                case EventLevel::Error:
                    return spdlog::level::err;
                case EventLevel::Off:
                    return spdlog::level::off;
            }

            return spdlog::level::info;
        }

    } // namespace

    SpdlogEventSink::SpdlogEventSink(std::shared_ptr<spdlog::logger> logger,
                                     std::unique_ptr<IEventSerializer> serializer) :
        logger_(std::move(logger)), serializer_(std::move(serializer)) {}

    void SpdlogEventSink::write(EventLevel level, std::string_view event_name, const json &fields) {
        logger_->log(to_spdlog_level(level), serializer_->serialize(level, event_name, fields));
    }

    void SpdlogEventSink::flush() { logger_->flush(); }

} // namespace cpgdb
