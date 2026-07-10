//
// Created by akos on 9. 7. 2026.
//

#ifndef CPGDB_LOGGINGSETUP_HPP
#define CPGDB_LOGGINGSETUP_HPP

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "EventLogger.hpp"

namespace cpgdb::logging {

namespace fs = std::filesystem;
using json = nlohmann::json;

struct LoggingOptions {
    // Relative paths are resolved against the project root.
    fs::path log_dir = "logs";

    // Optional nice name, for example:
    //   bfs_neo4j_debug
    std::string log_name;

    // Optional exact output file.
    // If this is set, it overrides log_dir + log_name.
    // Relative paths are also resolved against the project root.
    fs::path log_file;

    bool enabled = true;
};

// ---------------------------------------------------------
// Project root
// ---------------------------------------------------------

inline fs::path project_root() {
#ifdef CPGDB_PROJECT_ROOT
    return fs::path(CPGDB_PROJECT_ROOT);
#else
    // Fallback: useful if you forgot the CMake definition.
    return fs::current_path();
#endif
}

inline fs::path resolve_against_project_root(fs::path path) {
    if (path.is_absolute()) {
        return path;
    }

    return project_root() / path;
}

// ---------------------------------------------------------
// Filename helpers
// ---------------------------------------------------------

inline std::string timestamp_for_filename() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};

#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
    return oss.str();
}

inline std::string sanitize_filename(std::string name) {
    for (char& c : name) {
        const bool ok =
            std::isalnum(static_cast<unsigned char>(c)) ||
            c == '_' ||
            c == '-' ||
            c == '.';

        if (!ok) {
            c = '_';
        }
    }

    while (!name.empty() && name.front() == '.') {
        name.erase(name.begin());
    }

    if (name.empty()) {
        name = timestamp_for_filename();
    }

    return name;
}

inline fs::path with_jsonl_extension(fs::path path) {
    if (path.extension().empty()) {
        path += ".events.jsonl";
    }

    return path;
}

inline std::string run_id_from_log_file(const fs::path& path) {
    auto filename = path.filename().string();

    const std::string suffix = ".events.jsonl";

    if (filename.size() > suffix.size() &&
        filename.ends_with(suffix)) {
        filename.resize(filename.size() - suffix.size());
        return filename;
    }

    return path.stem().string();
}

// ---------------------------------------------------------
// Log path resolution
// ---------------------------------------------------------

inline fs::path resolve_log_file(const LoggingOptions& options) {
    if (!options.enabled) {
        return {};
    }

    if (!options.log_file.empty()) {
        auto file = resolve_against_project_root(options.log_file);
        file = with_jsonl_extension(file);

        if (file.has_parent_path()) {
            fs::create_directories(file.parent_path());
        }

        return file;
    }

    auto dir = resolve_against_project_root(options.log_dir);
    fs::create_directories(dir);

    std::string filename;

    if (!options.log_name.empty()) {
        filename = sanitize_filename(options.log_name);
    } else {
        filename = timestamp_for_filename();
    }

    auto file = dir / filename;
    return with_jsonl_extension(file);
}

// ---------------------------------------------------------
// Main factory
// ---------------------------------------------------------

inline std::unique_ptr<EventLogger> create_event_logger(
    const LoggingOptions& options
) {
    if (!options.enabled) {
        return nullptr;
    }

    const auto log_file = resolve_log_file(options);
    const auto run_id = run_id_from_log_file(log_file);

    return std::make_unique<EventLogger>(log_file);
}

} // namespace cpgdb::logging

#endif // CPGDB_LOGGINGSETUP_HPP