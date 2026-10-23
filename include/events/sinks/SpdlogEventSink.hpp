#pragma once

#include "events/sinks/IEventSerializer.hpp"
#include "events/sinks/IEventSink.hpp"
#include "observability/ObservabilityConfig.hpp"

#include <memory>
#include <spdlog/logger.h>
#include <string_view>
#include <vector>

namespace polygraph::events {

class SpdlogEventSink final : public IEventSink {
  public:
    SpdlogEventSink(std::shared_ptr<spdlog::logger> logger,
                    std::unique_ptr<IEventSerializer> serializer,
                    EventLevel minimum_level,
                    EventLevel flush_level,
                    observability::EventSelection selection);

    [[nodiscard]] bool accepts(EventLevel level, std::string_view event_name) const noexcept override;
    void write(EventLevel level, std::string_view event_name, const nlohmann::json& fields) override;
    void flush() override;

  private:
    [[nodiscard]] static bool matches_prefixes(
        std::string_view value,
        const std::vector<std::string>& prefixes) noexcept;

    std::shared_ptr<spdlog::logger> logger_;
    std::unique_ptr<IEventSerializer> serializer_;
    EventLevel minimum_level_;
    observability::EventSelection selection_;
};

} // namespace polygraph::events
