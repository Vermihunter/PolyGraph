/**
 * @file src/logging/LoggingSetup.cpp
 * @brief Implementation for logging setup in the event logging module.
 */

#include "logging/LoggingSetup.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace cpgdb::logging {
    namespace fs = std::filesystem;
    namespace {

        fs::path project_root() {
            return {
#ifdef CPGDB_PROJECT_ROOT
                    fs::path(CPGDB_PROJECT_ROOT)
#else
                    fs::current_path()
#endif
            };
        }

        fs::path resolve_against_project_root(fs::path path) {
            return path.is_absolute() ? std::move(path) : project_root() / path;
        }

        std::string timestamp_for_filename() {
            const auto now = std::chrono::system_clock::now();
            const std::time_t time = std::chrono::system_clock::to_time_t(now);
            std::tm local_time{};
#if defined(_WIN32)
            localtime_s(&local_time, &time);
#else
            localtime_r(&time, &local_time);
#endif
            std::ostringstream output;
            output << std::put_time(&local_time, "%Y-%m-%d_%H-%M-%S");
            return output.str();
        }

        std::string sanitize_filename(std::string name) {
            std::ranges::transform(name, name.begin(), [](unsigned char character) {
                return std::isalnum(character) || character == '_' || character == '-' || character == '.'
                               ? static_cast<char>(character)
                               : '_';
            });
            while (!name.empty() && name.front() == '.') {
                name.erase(name.begin());
            }
            return name.empty() ? timestamp_for_filename() : name;
        }

        fs::path with_jsonl_extension(fs::path path) {
            if (path.extension().empty()) {
                path += ".events.jsonl";
            }
            return path;
        }

        fs::path resolve_log_file(const LoggingOptions &options) {
            if (!options.enabled) {
                return {};
            }

            if (!options.log_file.empty()) {
                auto file = with_jsonl_extension(resolve_against_project_root(options.log_file));
                if (file.has_parent_path()) {
                    fs::create_directories(file.parent_path());
                }
                return file;
            }

            const auto directory = resolve_against_project_root(options.log_dir);
            fs::create_directories(directory);
            const std::string filename =
                    options.log_name.empty() ? timestamp_for_filename() : sanitize_filename(options.log_name);
            return with_jsonl_extension(directory / filename);
        }

    } // namespace

    std::unique_ptr<EventLogger> create_event_logger(const LoggingOptions &options) {
        if (!options.enabled) {
            return nullptr;
        }
        return std::make_unique<EventLogger>(resolve_log_file(options));
    }

} // namespace cpgdb::logging
