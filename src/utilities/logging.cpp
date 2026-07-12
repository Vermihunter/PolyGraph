/**
 * @file src/utilities/logging.cpp
 * @brief Implementation for logging in the utility module.
 */

#include "utilities/logging.hpp"

namespace cpgdb {
    logger_t logger;
    EventLogger *event_logger() noexcept { return logger.get(); }
} // namespace cpgdb
