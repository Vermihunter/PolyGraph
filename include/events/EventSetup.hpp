/**
 * @file include/events/EventSetup.hpp
 * @brief Build/install lifecycle for structured execution-event gathering.
 */
#pragma once

#include "observability/ObservabilityConfig.hpp"

#ifndef POLYGRAPH_ENABLE_EVENT_GATHERING
#define POLYGRAPH_ENABLE_EVENT_GATHERING 1
#endif

#if POLYGRAPH_ENABLE_EVENT_GATHERING
#include "events/EventCollector.hpp"
#include <memory>

namespace polygraph::events {
[[nodiscard]] std::unique_ptr<EventCollector>
create_collector(const observability::EventGatheringOptions& options);
void install_collector(const observability::EventGatheringOptions& options);
void shutdown() noexcept;
} // namespace polygraph::events
#else
namespace polygraph::events {
inline void install_collector(const observability::EventGatheringOptions&) noexcept {}
inline void shutdown() noexcept {}
} // namespace polygraph::events
#endif
