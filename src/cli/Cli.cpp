//
// Created by akos on 11. 7. 2026.
//

#include "cli/Cli.hpp"

#include "logging/LoggingSetup.hpp"
#include "utilities/logging.hpp"

namespace cpgdb::cli {

    static std::filesystem::path get_log_dir(const std::string &path) {
        std::filesystem::path log_file{path};

        if (log_file.is_relative()) {
            log_file = std::filesystem::path{CPGDB_PROJECT_ROOT} / log_file;
        }

        return log_file;
    }

    void add_log_arguments(CommandLine &program) {
        program.add_value_option("log-dir", {"--log-dir"}, "Directory for event logs", std::string{"logs"});

        program.add_value_option("log-name", {"--log-name"},
                                 "Log filename without extension; current date/time when omitted", std::string{});

        program.add_value_option("log-file", {"--log-file"}, "Exact log file path; overrides --log-dir and --log-name",
                                 std::string{});

        program.add_flag_option("no-event-log", {"--no-event-log"}, "Disable structured event logging");
    }

    logging::LoggingOptions parse_log_options(const CommandLine &args) {
        logging::LoggingOptions log_options;

        log_options.enabled = !args.bool_value("no-event-log");
        log_options.log_dir = get_log_dir(args.string_value("log-dir"));
        log_options.log_name = args.string_value("log-name");

        const auto &explicit_log_file = args.string_value("log-file");
        if (!explicit_log_file.empty()) {
            log_options.log_file = get_log_dir(explicit_log_file);
        }

        return log_options;
        // logger = cpgdb::logging::create_event_logger(log_options);
    }

} // namespace cpgdb::cli
