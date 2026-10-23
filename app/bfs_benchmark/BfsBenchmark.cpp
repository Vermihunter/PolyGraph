/**
 * @file app/bfs_benchmark/BfsBenchmark.cpp
 * @brief In-process repeated BFS benchmark orchestration.
 */

#include "BfsBenchmark.hpp"

#include "algorithms/bfs/BfsExecutor.hpp"
#include "cli/BfsCli.hpp"
#include "cli/Cli.hpp"
#include "cli/CurlGlobalGuard.hpp"
#include "cli/GraphRuntimeBuilder.hpp"
#include "events/EventSetup.hpp"
#include "events/Events.hpp"
#include "observability/ObservabilityConfig.hpp"
#include "logging/LoggingSetup.hpp"
#include "logging/Log.hpp"
#include "utilities/Profiling.hpp"
#include "utilities/json.hpp"
#include "utilities/uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace polygraph::benchmark {
namespace {

struct RunPaths {
    std::filesystem::path output_csv;
    std::filesystem::path event_log;
    std::filesystem::path stderr_log;
    std::filesystem::path metadata;
};

struct RunResult {
    std::size_t run_index{};
    std::string requested_source;
    std::string resolved_source;
    std::size_t repetition{};
    std::string status;
    double duration_ms{};
    std::size_t vertex_count{};
    std::string started_at;
    std::string finished_at;
    RunPaths paths;
    std::string error;
    bool resumed = false;
};


class ScopedLogger final {
  public:
    ScopedLogger(const observability::LoggingOptions& base, const std::filesystem::path& output_directory,
                 const bool use_benchmark_default_log) {
        auto options = base;
        if (use_benchmark_default_log) {
            for (auto& sink : options.sinks) {
                if (sink.type == observability::SinkType::BasicFile ||
                    sink.type == observability::SinkType::RotatingFile) {
                    sink.path = output_directory / "logs" / "benchmark.log";
                    sink.enabled = true;
                }
            }
        }
        logging::install_logger(options);
    }
    ScopedLogger(const ScopedLogger&) = delete;
    ScopedLogger& operator=(const ScopedLogger&) = delete;
    ~ScopedLogger() { logging::shutdown(); }
};

class ScopedEventCollector final {
  public:
    ScopedEventCollector(const observability::EventGatheringOptions& base, const std::filesystem::path& event_log) {
        auto options = base;
        options.sinks.clear();
        if (options.enabled) {
            observability::EventSinkOptions sink;
            sink.name = "benchmark-json";
            sink.type = observability::SinkType::BasicFile;
            sink.format = observability::EventFormat::Json;
            sink.minimum_level = options.minimum_level;
            sink.flush_level = EventLevel::Warn;
            sink.enabled = true;
            sink.color = false;
            sink.truncate = true;
            sink.path = event_log;
            options.sinks.push_back(std::move(sink));
        }
        events::install_collector(options);
    }

    ScopedEventCollector(const ScopedEventCollector&) = delete;
    ScopedEventCollector& operator=(const ScopedEventCollector&) = delete;
    ScopedEventCollector(ScopedEventCollector&&) = delete;
    ScopedEventCollector& operator=(ScopedEventCollector&&) = delete;

    ~ScopedEventCollector() { events::shutdown(); }
};

class ScopedStreamRedirect final {
  public:
    ScopedStreamRedirect(std::ostream& stream, std::streambuf* destination) : stream_(stream), original_(stream.rdbuf()) {
        stream_.rdbuf(destination);
    }

    ScopedStreamRedirect(const ScopedStreamRedirect&) = delete;
    ScopedStreamRedirect& operator=(const ScopedStreamRedirect&) = delete;
    ScopedStreamRedirect(ScopedStreamRedirect&&) = delete;
    ScopedStreamRedirect& operator=(ScopedStreamRedirect&&) = delete;

    ~ScopedStreamRedirect() { stream_.rdbuf(original_); }

  private:
    std::ostream& stream_;
    std::streambuf* original_;
};

[[nodiscard]] std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

[[nodiscard]] std::vector<std::string> split_csv(const std::string& raw) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= raw.size()) {
        const auto end = raw.find(',', begin);
        const auto value = trim(raw.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (!value.empty()) {
            result.push_back(value);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

[[nodiscard]] std::vector<std::string> read_start_uuids(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Could not open start UUID file: " + path.string());
    }

    std::vector<std::string> result;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        auto value = trim(line);
        if (value.empty() || value.starts_with('#')) {
            continue;
        }
        const auto comment = value.find('#');
        if (comment != std::string::npos) {
            value = trim(value.substr(0, comment));
        }
        if (value.empty()) {
            continue;
        }
        if (value.find_first_of(" \t") != std::string::npos) {
            throw std::runtime_error("Start UUID file contains whitespace inside an ID at line " +
                                     std::to_string(line_number) + ": " + path.string());
        }
        result.push_back(std::move(value));
    }
    return result;
}

[[nodiscard]] std::vector<std::string> start_uuids(const cli::CommandLine& command_line) {
    const auto& single = command_line.string_value("start-uuid");
    const auto& csv = command_line.string_value("start-uuids");
    const auto& file = command_line.string_value("start-uuids-file");
    const auto supplied = static_cast<int>(!single.empty()) + static_cast<int>(!csv.empty()) +
                          static_cast<int>(!file.empty());
    if (supplied != 1) {
        throw std::invalid_argument(
            "Provide exactly one of --start-uuid, --start-uuids, or --start-uuids-file");
    }

    std::vector<std::string> values;
    if (!single.empty()) {
        values.push_back(single);
    } else if (!csv.empty()) {
        values = split_csv(csv);
    } else {
        values = read_start_uuids(file);
    }
    if (values.empty()) {
        throw std::invalid_argument("The start UUID list is empty");
    }

    std::unordered_set<std::string> seen;
    std::vector<std::string> unique;
    unique.reserve(values.size());
    for (auto& value : values) {
        if (seen.insert(value).second) {
            unique.push_back(std::move(value));
        }
    }
    return unique;
}

[[nodiscard]] std::size_t positive_size(const cli::CommandLine& command_line, const std::string_view key,
                                        const bool allow_zero = false) {
    const int value = command_line.int_value(key);
    if (value < 0 || (!allow_zero && value == 0)) {
        throw std::invalid_argument("--" + std::string(key) +
                                    (allow_zero ? " must be zero or greater" : " must be greater than zero"));
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] std::string safe_component(const std::string& value) {
    std::string result;
    result.reserve(std::min<std::size_t>(value.size(), 96));
    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('_');
        }
        if (result.size() == 96) {
            break;
        }
    }
    if (result.empty() || result == "." || result == "..") {
        throw std::invalid_argument("Start UUID cannot be converted to a safe path component: " + value);
    }
    return result;
}

[[nodiscard]] std::string repetition_name(const std::size_t repetition) {
    std::ostringstream output;
    output << "rep" << std::setw(3) << std::setfill('0') << repetition;
    return output.str();
}

[[nodiscard]] RunPaths make_run_paths(const std::filesystem::path& output_directory,
                                      const std::string& source_uuid, const std::size_t repetition) {
    const auto source = safe_component(source_uuid);
    const auto name = repetition_name(repetition);
    return RunPaths{
        .output_csv = output_directory / "outputs" / source / (name + ".csv"),
        .event_log = output_directory / "events" / source / (name + ".events.jsonl"),
        .stderr_log = output_directory / "logs" / source / (name + ".stderr.log"),
        .metadata = output_directory / "runs" / source / (name + ".json"),
    };
}

void create_parent_directories(const RunPaths& paths) {
    std::filesystem::create_directories(paths.output_csv.parent_path());
    std::filesystem::create_directories(paths.event_log.parent_path());
    std::filesystem::create_directories(paths.stderr_log.parent_path());
    std::filesystem::create_directories(paths.metadata.parent_path());
}

void clear_run_artifacts(const RunPaths& paths) {
    std::error_code error;
    std::filesystem::remove(paths.output_csv, error);
    error.clear();
    std::filesystem::remove(paths.event_log, error);
    error.clear();
    std::filesystem::remove(paths.stderr_log, error);
    error.clear();
    std::filesystem::remove(paths.metadata, error);
}

void prepare_output_directory(const std::filesystem::path& output_directory, const bool overwrite,
                              const bool resume) {
    const bool exists = std::filesystem::exists(output_directory);
    if (exists && !std::filesystem::is_directory(output_directory)) {
        throw std::runtime_error("Benchmark output path is not a directory: " + output_directory.string());
    }

    const bool nonempty = exists && std::filesystem::directory_iterator(output_directory) !=
                                        std::filesystem::directory_iterator{};
    if (nonempty && overwrite) {
        std::filesystem::remove_all(output_directory);
    } else if (nonempty && !resume) {
        throw std::runtime_error("Benchmark output directory is not empty: " + output_directory.string() +
                                 " (use --overwrite or --resume)");
    }
    std::filesystem::create_directories(output_directory);
}

[[nodiscard]] std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&raw, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

void replace_file(const std::filesystem::path& temporary, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::remove(target, error);
    error.clear();
    std::filesystem::rename(temporary, target, error);
    if (error) {
        throw std::runtime_error("Could not replace " + target.string() + ": " + error.message());
    }
}

void write_json_atomic(const std::filesystem::path& path, const json& value) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Could not write " + temporary.string());
        }
        output << value.dump(2) << '\n';
        output.flush();
        if (!output) {
            throw std::runtime_error("Failed while writing " + temporary.string());
        }
    }
    replace_file(temporary, path);
}

[[nodiscard]] json run_json(const RunResult& result) {
    return json{
        {"runIndex", result.run_index},
        {"requestedSourceUuid", result.requested_source},
        {"resolvedSourceUuid", result.resolved_source},
        {"repetition", result.repetition},
        {"status", result.status},
        {"durationMs", result.duration_ms},
        {"vertexCount", result.vertex_count},
        {"startedAt", result.started_at},
        {"finishedAt", result.finished_at},
        {"resumed", result.resumed},
        {"error", result.error},
        {"files",
         {{"outputCsv", result.paths.output_csv.string()},
          {"eventLog", result.paths.event_log.string()},
          {"stderrLog", result.paths.stderr_log.string()},
          {"metadata", result.paths.metadata.string()}}},
    };
}

[[nodiscard]] std::optional<RunResult> load_completed_run(const RunPaths& paths) {
    if (!std::filesystem::is_regular_file(paths.metadata) ||
        !std::filesystem::is_regular_file(paths.output_csv)) {
        return std::nullopt;
    }

    try {
        std::ifstream input(paths.metadata);
        const json value = json::parse(input);
        if (value.value("status", std::string{}) != "success") {
            return std::nullopt;
        }
        return RunResult{
            .run_index = value.value("runIndex", std::size_t{}),
            .requested_source = value.value("requestedSourceUuid", std::string{}),
            .resolved_source = value.value("resolvedSourceUuid", std::string{}),
            .repetition = value.value("repetition", std::size_t{}),
            .status = "success",
            .duration_ms = value.value("durationMs", 0.0),
            .vertex_count = value.value("vertexCount", std::size_t{}),
            .started_at = value.value("startedAt", std::string{}),
            .finished_at = value.value("finishedAt", std::string{}),
            .paths = paths,
            .error = {},
            .resumed = true,
        };
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::string csv_field(const std::string& value) {
    if (value.find_first_of(";\"\r\n") == std::string::npos) {
        return value;
    }
    std::string escaped = "\"";
    for (const char character : value) {
        escaped.push_back(character);
        if (character == '"') {
            escaped.push_back('"');
        }
    }
    escaped.push_back('"');
    return escaped;
}

void write_measurements(const std::filesystem::path& path, const std::vector<RunResult>& results) {
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Could not write " + temporary.string());
        }
        output << "run_index;requested_source_uuid;resolved_source_uuid;repetition;status;duration_ms;"
                  "vertex_count;resumed;output_csv;event_log;stderr_log;metadata;error\n";
        output << std::fixed << std::setprecision(3);
        for (const auto& result : results) {
            output << result.run_index << ';' << csv_field(result.requested_source) << ';'
                   << csv_field(result.resolved_source) << ';' << result.repetition << ';' << result.status << ';'
                   << result.duration_ms << ';' << result.vertex_count << ';' << (result.resumed ? "true" : "false")
                   << ';' << csv_field(result.paths.output_csv.string()) << ';'
                   << csv_field(result.paths.event_log.string()) << ';'
                   << csv_field(result.paths.stderr_log.string()) << ';'
                   << csv_field(result.paths.metadata.string()) << ';' << csv_field(result.error) << '\n';
        }
        output.flush();
        if (!output) {
            throw std::runtime_error("Failed while writing " + temporary.string());
        }
    }
    replace_file(temporary, path);
}

[[nodiscard]] json make_summary(const std::vector<RunResult>& results, const double setup_duration_ms,
                                const std::size_t requested_run_count) {
    std::size_t successful = 0;
    std::size_t failed = 0;
    std::map<std::string, std::vector<double>> durations;
    std::map<std::string, std::size_t> vertex_counts;

    for (const auto& result : results) {
        if (result.status == "success") {
            ++successful;
            durations[result.requested_source].push_back(result.duration_ms);
            vertex_counts[result.requested_source] = result.vertex_count;
        } else {
            ++failed;
        }
    }

    json by_source = json::object();
    for (const auto& [source, values] : durations) {
        const double total = std::accumulate(values.begin(), values.end(), 0.0);
        const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
        by_source[source] = {
            {"successfulRuns", values.size()},
            {"meanDurationMs", total / static_cast<double>(values.size())},
            {"minDurationMs", *minimum},
            {"maxDurationMs", *maximum},
            {"vertexCount", vertex_counts.at(source)},
        };
    }

    return json{
        {"runtimeLoadDurationMs", setup_duration_ms},
        {"requestedRunCount", requested_run_count},
        {"recordedRunCount", results.size()},
        {"successfulRunCount", successful},
        {"failedRunCount", failed},
        {"bySource", std::move(by_source)},
        {"generatedAt", utc_timestamp()},
    };
}

[[nodiscard]] json effective_config(const cli::GraphRuntimeConfig& runtime_config, const BfsOptions& bfs_options,
                                    const cli::CommandLine& command_line,
                                    const std::vector<std::string>& sources, const std::size_t repetitions,
                                    const std::size_t cooldown_ms, const std::filesystem::path& output_directory) {
    json max_depth = nullptr;
    if (bfs_options.max_depth) {
        max_depth = *bfs_options.max_depth;
    }

    json bfs = json::object();
    bfs["maxDepth"] = std::move(max_depth);
    bfs["direction"] = command_line.string_value("direction");
    bfs["allowedEdgeTypes"] = command_line.string_value("allowed-edge-types");
    bfs["allowedTargetNodeTypes"] = command_line.string_value("allowed-target-node-types");
    bfs["returnPredecessors"] = bfs_options.return_predecessors;
    bfs["allowNativePushdown"] = bfs_options.allow_native_pushdown;
    bfs["allowPartialAcceleration"] = bfs_options.allow_partial_acceleration;
    bfs["neighborBatchSize"] = bfs_options.neighbor_batch_size;
    bfs["neo4jMaxConcurrentRequests"] = bfs_options.neo4j_max_concurrent_requests;
    bfs["arangoMaxConcurrentRequests"] = bfs_options.arango_max_concurrent_requests;
    bfs["arangoRequestStrategy"] = std::string(to_string(bfs_options.arango_request_strategy));
    bfs["arangoSingleRequestMaxFrontier"] = bfs_options.arango_single_request_max_frontier;
    bfs["backendRequestRetries"] = bfs_options.backend_request_retries;
    bfs["backendRetryDelayMs"] = bfs_options.backend_retry_delay_ms;
    bfs["threadCount"] = bfs_options.thread_count;

    json result = json::object();
    result["domain"] = command_line.string_value("domain");
    result["logicalGraphSchema"] = runtime_config.logical_graph_schema_path;
    result["data"] = runtime_config.data_path;
    result["backendConfig"] = runtime_config.backend_config_path.value_or(std::string{});
    result["nativeIdMap"] = runtime_config.native_id_map_path.value_or(std::string{});
    result["cachePlan"] = runtime_config.cache_plan_path.value_or(std::string{});
    result["startUuids"] = sources;
    result["repetitions"] = repetitions;
    result["cooldownMs"] = cooldown_ms;
    result["outputDirectory"] = output_directory.string();
    result["bfs"] = std::move(bfs);
    return result;
}

void write_bfs_csv(const std::filesystem::path& path, const BfsResult& result) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not open BFS output file: " + path.string());
    }
    cli::write_bfs_distances_csv(output, result);
    output.flush();
    if (!output) {
        throw std::runtime_error("Failed while writing BFS output file: " + path.string());
    }
}

} // namespace

void add_bfs_benchmark_arguments(cli::CommandLine& command_line) {
    command_line.add_value_option("start-uuids", {"--start-uuids"},
                                  "Comma-separated BFS source UUIDs", std::string{});
    command_line.add_value_option("start-uuids-file", {"--start-uuids-file"},
                                  "Text file containing one BFS source UUID per line", std::string{});
    command_line.add_value_option("repetitions", {"--repetitions"},
                                  "Measured repetitions for each source UUID", std::string{"1"});
    command_line.add_value_option("output-dir", {"--output-dir"}, "Benchmark output directory", std::nullopt,
                                  true);
    command_line.add_value_option("cooldown-ms", {"--cooldown-ms"},
                                  "Delay between BFS runs in milliseconds", std::string{"0"});
    command_line.add_flag_option("overwrite", {"--overwrite"},
                                 "Delete an existing benchmark output directory before running");
    command_line.add_flag_option("resume", {"--resume"},
                                 "Skip runs already recorded successfully in the output directory");
    command_line.add_flag_option("fail-fast", {"--fail-fast"}, "Stop after the first failed BFS run");
}

int run_bfs_benchmark(const cli::CommandLine& command_line) {
    const auto sources = start_uuids(command_line);
    const auto repetitions = positive_size(command_line, "repetitions");
    const auto cooldown_ms = positive_size(command_line, "cooldown-ms", true);
    const auto output_directory = std::filesystem::absolute(command_line.string_value("output-dir")).lexically_normal();
    const bool overwrite = command_line.bool_value("overwrite");
    const bool resume = command_line.bool_value("resume");
    if (overwrite && resume) {
        throw std::invalid_argument("--overwrite and --resume are mutually exclusive");
    }
    prepare_output_directory(output_directory, overwrite, resume);

    const auto runtime_config = cli::parse_runtime_config(command_line);
    const auto bfs_options = cli::parse_bfs_options(command_line);
    const auto observability = cli::parse_observability_config(command_line);
    const bool use_benchmark_default_log = command_line.string_value("application-log-file").empty();
    ScopedLogger benchmark_logger(observability.logging, output_directory, use_benchmark_default_log);
    LOG_INFO("Benchmark setup: output directory {}", output_directory.string());
    if (runtime_config.cache_plan_path) {
        LOG_INFO("Benchmark setup: selected cache plan {}", *runtime_config.cache_plan_path);
    }
    write_json_atomic(output_directory / "effective-config.json",
                      effective_config(runtime_config, bfs_options, command_line, sources, repetitions, cooldown_ms,
                                       output_directory));

    runtime::CurlGlobalGuard curl;
    const auto setup_started = std::chrono::steady_clock::now();
    std::shared_ptr<DatabaseContext> context;
    {
        const auto setup_log = output_directory / "events" / "setup.events.jsonl";
        std::filesystem::create_directories(setup_log.parent_path());
        ScopedEventCollector setup_events(observability.events, setup_log);
        EVENT_INFO("bfs.benchmark.runtime_load.starting", {"source_count", sources.size()},
                   {"repetitions", repetitions});
        context = cli::GraphRuntimeBuilder::build(runtime_config);
        EVENT_INFO("bfs.benchmark.runtime_load.completed",
                   {"gpu_runtime_loaded", context->gpu_runtime != nullptr});
    }
    LOG_INFO("Benchmark setup: graph runtime ready");
    const double setup_duration_ms = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - setup_started)
                                         .count();

    const auto expand_spec = cli::parse_expand_spec(command_line, *context->schema);
    BfsExecutor executor(*context->placement_catalog);

    const std::size_t requested_run_count = sources.size() * repetitions;
    std::vector<RunResult> results;
    results.reserve(requested_run_count);
    std::size_t run_index = 0;
    bool stop = false;

    for (const auto& source : sources) {
        for (std::size_t repetition = 1; repetition <= repetitions; ++repetition) {
            ++run_index;
            const auto paths = make_run_paths(output_directory, source, repetition);
            create_parent_directories(paths);

            if (resume) {
                if (auto previous = load_completed_run(paths)) {
                    previous->run_index = run_index;
                    previous->requested_source = source;
                    previous->repetition = repetition;
                    results.push_back(std::move(*previous));
                    write_measurements(output_directory / "measurements.csv", results);
                    std::cout << "[" << run_index << '/' << requested_run_count << "] " << source << " repetition "
                              << repetition << " already complete\n";
                    continue;
                }
            }

            clear_run_artifacts(paths);

            RunResult run{
                .run_index = run_index,
                .requested_source = source,
                .resolved_source = {},
                .repetition = repetition,
                .status = "failed",
                .duration_ms = 0.0,
                .vertex_count = 0,
                .started_at = utc_timestamp(),
                .finished_at = {},
                .paths = paths,
                .error = {},
                .resumed = false,
            };

            std::ofstream stderr_output(paths.stderr_log, std::ios::trunc);
            if (!stderr_output) {
                throw std::runtime_error("Could not open per-run stderr log: " + paths.stderr_log.string());
            }

            {
                ScopedStreamRedirect stderr_redirect(std::cerr, stderr_output.rdbuf());
                ScopedEventCollector run_events(observability.events, paths.event_log);
                EVENT_INFO("bfs.benchmark.run.starting", {"run_index", run_index},
                           {"requested_source_uuid", source}, {"repetition", repetition});

                try {
                    const NodeRef source_ref = context->node_ref(global_id_from_string(source));
                    run.resolved_source = global_id_to_string(source_ref.global_id);

                    const auto started = std::chrono::steady_clock::now();
                    const BfsResult result = executor.run(source_ref, expand_spec, bfs_options);
                    run.duration_ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                    run.vertex_count = result.distance.size();
                    write_bfs_csv(paths.output_csv, result);
                    run.status = "success";

                    EVENT_INFO("bfs.benchmark.run.completed", {"run_index", run_index},
                               {"requested_source_uuid", source},
                               {"resolved_source_uuid", run.resolved_source}, {"repetition", repetition},
                               {"duration_ms", run.duration_ms}, {"vertices", run.vertex_count});
                } catch (const std::exception& error) {
                    run.error = error.what();
                    EVENT_ERROR("bfs.benchmark.run.failed", {"run_index", run_index},
                                {"requested_source_uuid", source}, {"repetition", repetition},
                                {"error", run.error});
                    stderr_output << "polygraph-bfs-benchmark: " << run.error << '\n';
                    prof::Profiler::dump(stderr_output);
                } catch (...) {
                    run.error = "unknown exception";
                    EVENT_ERROR("bfs.benchmark.run.failed", {"run_index", run_index},
                                {"requested_source_uuid", source}, {"repetition", repetition},
                                {"error", run.error});
                    stderr_output << "polygraph-bfs-benchmark: unknown exception\n";
                    prof::Profiler::dump(stderr_output);
                }
            }

            stderr_output.flush();
            run.finished_at = utc_timestamp();
            write_json_atomic(paths.metadata, run_json(run));
            results.push_back(run);
            write_measurements(output_directory / "measurements.csv", results);
            write_json_atomic(output_directory / "summary.json",
                              make_summary(results, setup_duration_ms, requested_run_count));

            std::cout << '[' << run_index << '/' << requested_run_count << "] " << source << " repetition "
                      << repetition << ": " << run.status;
            if (run.status == "success") {
                std::cout << " (" << std::fixed << std::setprecision(3) << run.duration_ms << " ms, "
                          << run.vertex_count << " vertices)";
            } else {
                std::cout << " (" << run.error << ')';
            }
            std::cout << '\n';

            if (run.status != "success" && command_line.bool_value("fail-fast")) {
                stop = true;
                break;
            }
            if (cooldown_ms > 0 && run_index < requested_run_count) {
                std::this_thread::sleep_for(std::chrono::milliseconds(cooldown_ms));
            }
        }
        if (stop) {
            break;
        }
    }

    write_measurements(output_directory / "measurements.csv", results);
    write_json_atomic(output_directory / "summary.json",
                      make_summary(results, setup_duration_ms, requested_run_count));

    LOG_INFO("Benchmark complete: {} recorded runs", results.size());
    const bool failed = std::ranges::any_of(results, [](const RunResult& result) {
        return result.status != "success";
    });
    return failed ? 1 : 0;
}

} // namespace polygraph::benchmark
