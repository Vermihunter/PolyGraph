/**
 * @file include/logging/LoggingSetup.hpp
 * @brief Declarations for logging setup in the event logging module.
 */

#ifndef CPGDB_LOGGING_LOGGING_SETUP_HPP
#define CPGDB_LOGGING_LOGGING_SETUP_HPP

#include "logging/EventLogger.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace cpgdb::logging {

struct LoggingOptions {
    std::filesystem::path log_dir = "logs";
    std::string log_name;
    std::filesystem::path log_file;
    bool enabled = true;
};

[[nodiscard]] std::unique_ptr<EventLogger> create_event_logger(const LoggingOptions& options);

} // namespace cpgdb::logging

#endif // CPGDB_LOGGING_LOGGING_SETUP_HPP
