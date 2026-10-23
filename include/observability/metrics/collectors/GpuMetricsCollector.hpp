#pragma once

#include "observability/metrics/collectors/IMetricsCollector.hpp"

#include <memory>
#include <string>

namespace polygraph::observability::metrics {

class GpuMetricsCollector final : public IMetricsCollector {
  public:
    explicit GpuMetricsCollector(unsigned int device_index = 0);
    ~GpuMetricsCollector() override;

    GpuMetricsCollector(const GpuMetricsCollector&) = delete;
    GpuMetricsCollector& operator=(const GpuMetricsCollector&) = delete;
    GpuMetricsCollector(GpuMetricsCollector&&) noexcept;
    GpuMetricsCollector& operator=(GpuMetricsCollector&&) noexcept;

    [[nodiscard]] std::string_view name() const noexcept override { return collector_name_; }
    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] MetricValues collect(std::span<const std::string> requested) override;

  private:
    struct State;
    std::unique_ptr<State> state_;
    std::string collector_name_;
};

} // namespace polygraph::observability::metrics
