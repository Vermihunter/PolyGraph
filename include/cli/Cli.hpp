//
// Created by akos on 11. 7. 2026.
//

#ifndef CPGDB_CLI_HPP
#define CPGDB_CLI_HPP
#include "CommandLine.hpp"
#include "logging/LoggingSetup.hpp"

namespace cpgdb::cli {
logging::LoggingOptions parse_log_options(const CommandLine& args);
void add_log_arguments(CommandLine& program);
} // namespace cpgdb::cli

#endif // CPGDB_CLI_HPP
