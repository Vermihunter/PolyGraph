/**
 * @file include/logging/LoggingSetup.hpp
 * @brief Build/install lifecycle for operational application logging.
 */
#pragma once

#include "observability/ObservabilityConfig.hpp"

namespace polygraph::logging {

void install_logger(const observability::LoggingOptions& options);
void shutdown() noexcept;

} // namespace polygraph::logging
