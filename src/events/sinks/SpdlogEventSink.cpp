#include "events/sinks/SpdlogEventSink.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>
#include <utility>

namespace polygraph::events {
    namespace {
        [[nodiscard]] spdlog::level::level_enum to_spdlog_level(const EventLevel level) noexcept {
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
            return spdlog::level::off;
        }
    } // namespace

    SpdlogEventSink::SpdlogEventSink(std::shared_ptr<spdlog::logger> logger,
                                     std::unique_ptr<IEventSerializer> serializer, const EventLevel minimum_level,
                                     const EventLevel flush_level, observability::EventSelection selection) :
        logger_(std::move(logger)), serializer_(std::move(serializer)), minimum_level_(minimum_level),
        selection_(std::move(selection)) {
        logger_->set_level(to_spdlog_level(minimum_level));
        logger_->flush_on(to_spdlog_level(flush_level));
    }

    bool SpdlogEventSink::matches_prefixes(const std::string_view value,
                                           const std::vector<std::string> &prefixes) noexcept {
        return std::ranges::any_of(prefixes, [&](const auto &prefix) { return value.starts_with(prefix); });
    }

    bool SpdlogEventSink::accepts(const EventLevel level, const std::string_view event_name) const noexcept {
        if (minimum_level_ == EventLevel::Off || level < minimum_level_) {
            return false;
        }
        if (!selection_.include_prefixes.empty() && !matches_prefixes(event_name, selection_.include_prefixes)) {
            return false;
        }
        return !matches_prefixes(event_name, selection_.exclude_prefixes);
    }

    void SpdlogEventSink::write(const EventLevel level, const std::string_view event_name,
                                const nlohmann::json &fields) {
        logger_->log(to_spdlog_level(level), serializer_->serialize(level, event_name, fields));
    }

    void SpdlogEventSink::flush() { logger_->flush(); }

} // namespace polygraph::events
