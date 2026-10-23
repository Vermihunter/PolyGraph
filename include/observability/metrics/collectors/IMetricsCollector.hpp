/**
 * @file include/observability/metrics/collectors/IMetricsCollector.hpp
 * @brief Pull-time system metrics collector interface.
 */

#pragma once

#include "observability/metrics/MetricTypes.hpp"

#include <span>
#include <string>
#include <string_view>

namespace polygraph::observability::metrics {

class IMetricsCollector {
  public:
    virtual ~IMetricsCollector() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool available() const noexcept = 0;
    [[nodiscard]] virtual MetricValues collect(std::span<const std::string> requested) = 0;
};

} // namespace polygraph::observability::metrics
