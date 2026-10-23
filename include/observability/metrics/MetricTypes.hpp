/**
 * @file include/observability/metrics/MetricTypes.hpp
 * @brief Common collector output types.
 */

#pragma once

#include <string>
#include <unordered_map>

namespace polygraph::observability::metrics {

using MetricValues = std::unordered_map<std::string, double>;

} // namespace polygraph::observability::metrics
