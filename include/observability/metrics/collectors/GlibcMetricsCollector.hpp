#pragma once

#include "observability/metrics/collectors/IMetricsCollector.hpp"

namespace polygraph::observability::metrics {

class GlibcMetricsCollector final : public IMetricsCollector {
  public:
    [[nodiscard]] std::string_view name() const noexcept override { return "glibc"; }
    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] MetricValues collect(std::span<const std::string> requested) override;
};

} // namespace polygraph::observability::metrics
