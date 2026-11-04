#include <cerrno>
#include <charconv>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <unistd.h>
#include "ingestion/Import.hpp"

namespace {
    using namespace polygraph::ingestion;
    constexpr auto help = R"(PolyGraph standalone native bulk ingestion (C++23)

polygraph_bulk_import prepare --adapter <ldbc-snb|polygraph-jsonl>
  --input <directory|file> --placement-config <placement.json> --output-dir <directory>
  [--partitioner none] [--seed 42] [--threads 3] [--memory-limit-mb 256]
  [--chunk-size-mb 64] [--max-record-mb 8]
  [--partition-file <assignments.jsonl> --partition-map <partition-backends.json>]
    prepare is offline: never connects to databases. Existing valid artifacts/checkpoints
    are reused only when input and configuration SHA-256 hashes match.
    KaMinPar is unavailable in this release; requests fail explicitly.
    External partitions map node source keys to partitions, then partitions to backends.
    Edges keep their independently configured schema ownership.
    memory-limit-mb budgets SQLite caches and record buffers, not a process RSS hard limit.

polygraph_bulk_import load --adapter <ldbc-snb|polygraph-jsonl> --input <directory|file>
  --placement-config <placement.json> --docker [--keep-running]
  [--output-dir <fresh-directory>] [--generation-name <readable-prefix>]
  [--threads 3] [--memory-limit-mb 256] [--chunk-size-mb 64] [--max-record-mb 8]
  [--partitioner none] [--concurrency 3] [--startup-timeout-seconds 180]
  [--neo-image <image>] [--arango-image <image>] [--pg-image <image>]
  [--skip-verification] [--verify-bfs] [--bfs-binary <polygraph_bfs>] [--bfs-max-vertices 100000]
    One command: prepare, strict verify, fresh isolated Docker resources, native
    execution and live verification. --docker authorizes creation of new owned resources.
    --keep-running retains imported services; otherwise owned resources are removed.
    Prints endpoints, counts, private credentials location and ownership-checked cleanup.
    Uses Python 3.10+ standard-library lifecycle helpers and Docker; no Compose project.
    Failures retain logs, stopped owned resources and import journals. No implicit retries.
    Image defaults prefer cached repository images, then fetch official images if missing.
    --verify-bfs additionally compares a depth-two distributed BFS with the source index.
    --skip-verification imports without exhaustive artifact/live/BFS correctness checks.
    Checksums, fresh-target safeguards and importer error/count checks remain enabled.
    Successful unchecked loads report imported, verification skipped; use verify-load later.

polygraph_bulk_import verify-load --output-dir <existing-load-directory>
  [--verify-bfs] [--bfs-binary <polygraph_bfs>] [--bfs-max-vertices 100000]
    Uses saved Docker ownership, targets and private credentials. Runs strict artifact
    and exact live verification without importing or stopping services; optionally BFS.
    Services must be running. Writes verification-result.json with separate elapsed time.

polygraph_bulk_import verify --manifest <manifest.json> [--strict]
    Checks sizes and SHA-256; strict also checks the endpoint index and reconstructs
    native representations in bounded memory (requires temporary disk space).

polygraph_bulk_import execute --manifest <manifest.json> --target-config <targets.json>
  --confirm-create [--concurrency 3] [--skip-verification]
    Requires isolated fresh named targets. Never drops databases or resets volumes.
    All preflights must succeed before imports start. Failed imports are never retried.
    Native executors: libpq COPY, Arango HTTP import, offline neo4j-admin runner.
    Neo4j start_command and an online endpoint enable final verification.
    --skip-verification records imported/unverified targets and skips exhaustive checks.

polygraph_bulk_import verify-live --manifest <manifest.json> --target-config <targets.json>
    Verifies exact node/edge/proxy counts, canonical IDs and endpoint traversal.
    Writes live-verification.json; does not rerun an importer or property repairs.

--help / -h  Show this help (valid with any command).
Exit codes: 0 success, 2 invalid arguments/configuration, 3 preparation/verification error,
            4 execution/live verification failure, 5 unexpected I/O/internal error.
See docs/guides/BULK_INGESTION.md for target configuration and safety/recovery rules.
)";
    struct Arguments {
        std::map<std::string, std::string> values;
        std::set<std::string> flags;
        std::string require(const std::string &key) const {
            const auto it = values.find(key);
            if (it == values.end())
                throw Error("Missing required option " + key);
            return it->second;
        }
        std::string get(const std::string &key, const std::string &fallback) const {
            const auto it = values.find(key);
            return it == values.end() ? fallback : it->second;
        }
        unsigned number(const std::string &key, unsigned fallback) const {
            if (!values.contains(key))
                return fallback;
            const auto &text = values.at(key);
            unsigned result = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
            if (error != std::errc{} || end != text.data() + text.size())
                throw Error("Expected unsigned integer for " + key);
            return result;
        }
    };
    Arguments parse(int argc, char **argv, const std::string &command) {
        std::set<std::string> allowed;
        if (command == "prepare")
            allowed = {"--adapter",       "--input",         "--placement-config", "--output-dir",
                       "--partitioner",   "--seed",          "--threads",          "--memory-limit-mb",
                       "--chunk-size-mb", "--max-record-mb", "--partition-file",   "--partition-map"};
        else if (command == "load")
            allowed = {"--adapter",          "--input",
                       "--placement-config", "--docker",
                       "--keep-running",     "--output-dir",
                       "--generation-name",  "--threads",
                       "--memory-limit-mb",  "--chunk-size-mb",
                       "--max-record-mb",    "--partitioner",
                       "--concurrency",      "--startup-timeout-seconds",
                       "--neo-image",        "--arango-image",
                       "--pg-image",         "--verify-bfs",
                       "--bfs-binary",       "--bfs-max-vertices",
                       "--skip-verification"};
        else if (command == "verify-load")
            allowed = {"--output-dir", "--verify-bfs", "--bfs-binary", "--bfs-max-vertices"};
        else if (command == "verify")
            allowed = {"--manifest", "--strict"};
        else if (command == "execute")
            allowed = {"--manifest", "--target-config", "--confirm-create", "--concurrency", "--skip-verification"};
        else if (command == "verify-live")
            allowed = {"--manifest", "--target-config"};
        else
            throw Error("Unknown command: " + command);
        Arguments result;
        for (int i = 2; i < argc; ++i) {
            const std::string key = argv[i];
            if (!allowed.contains(key))
                throw Error("Unknown option for " + command + ": " + key);
            if (result.flags.contains(key) || result.values.contains(key))
                throw Error("Duplicate option: " + key);
            if (key == "--strict" || key == "--confirm-create" || key == "--docker" || key == "--keep-running" ||
                key == "--verify-bfs" || key == "--skip-verification")
                result.flags.insert(key);
            else {
                if (i + 1 == argc || std::string_view(argv[i + 1]).starts_with("--"))
                    throw Error("Missing value for " + key);
                result.values[key] = argv[++i];
            }
        }
        return result;
    }
} // namespace
int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h") {
            std::cout << help;
            return 0;
        }
    if (argc < 2) {
        std::cerr << help;
        return 2;
    }
    const std::string command = argv[1];
    Arguments args;
    try {
        args = parse(argc, argv, command);
    } catch (const std::exception &e) {
        std::cerr << "Arguments: " << e.what() << "\nUse --help for options.\n";
        return 2;
    }
    try {
        if (command == "load" || command == "verify-load") {
            if (command == "load") {
                if (!args.flags.contains("--docker"))
                    throw Error("load requires --docker to create an isolated fresh environment");
                (void) args.require("--adapter");
                (void) args.require("--input");
                (void) args.require("--placement-config");
            } else
                (void) args.require("--output-dir");
            const auto executable = fs::canonical("/proc/self/exe").string();
            const auto script = fs::path(POLYGRAPH_PROJECT_ROOT) / "tools/ingestion/docker_load.py";
            if (!fs::is_regular_file(script))
                throw Error("Docker lifecycle helper is missing: " + script.string());
            std::vector<std::string> command_args{"python3", script.string(), command, "--binary", executable};
            for (int i = 2; i < argc; ++i)
                command_args.emplace_back(argv[i]);
            std::vector<char *> native_args;
            for (auto &arg: command_args)
                native_args.push_back(arg.data());
            native_args.push_back(nullptr);
            // Preserve streaming progress and exit status; argv is never interpreted by a shell.
            ::execvp(native_args.front(), native_args.data());
            throw Error("Cannot start Docker lifecycle helper (requires Python 3.10+): " +
                        std::string(std::strerror(errno)));
        } else if (command == "prepare") {
            PrepareOptions options;
            options.adapter = args.require("--adapter");
            options.input = args.require("--input");
            options.placement_config = args.require("--placement-config");
            options.output_dir = args.require("--output-dir");
            options.partitioner = args.get("--partitioner", "none");
            options.seed = args.number("--seed", 42);
            options.threads = args.number("--threads", 3);
            options.memory_limit_mb = args.number("--memory-limit-mb", 256);
            options.chunk_size_mb = args.number("--chunk-size-mb", 64);
            options.max_record_mb = args.number("--max-record-mb", 8);
            options.partition_file = args.get("--partition-file", "");
            options.partition_map = args.get("--partition-map", "");
            const auto manifest =
                    prepare(options, [](const std::string &stage) { std::cerr << "prepare: " << stage << '\n'; });
            std::cout << Json{{"state", "prepared"},
                              {"generation_id", manifest.at("generation_id")},
                              {"counts", manifest.at("counts")},
                              {"manifest", (options.output_dir / "manifest.json").string()}}
                                 .dump(2)
                      << '\n';
        } else if (command == "verify") {
            (void) verify(args.require("--manifest"), args.flags.contains("--strict"));
            std::cout << "Artifacts verified\n";
        } else if (command == "execute") {
            execute(args.require("--manifest"), args.require("--target-config"),
                    args.flags.contains("--confirm-create"), args.number("--concurrency", 3), {},
                    args.flags.contains("--skip-verification") ? VerificationMode::none : VerificationMode::full);
            std::cout << (args.flags.contains("--skip-verification")
                                  ? "All native imports completed; correctness verification skipped\n"
                                  : "All native imports and live validation succeeded\n");
        } else
            std::cout << verify_live(args.require("--manifest"), args.require("--target-config")).dump(2) << '\n';
        return 0;
    } catch (const Error &e) {
        std::cerr << "Bulk ingestion: " << e.what() << '\n';
        return command == "execute" || command == "verify-live" || command == "load" || command == "verify-load" ? 4
                                                                                                                 : 3;
    } catch (const Json::exception &e) {
        std::cerr << "Invalid configuration/record: " << e.what() << '\n';
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "Bulk ingestion I/O/internal error: " << e.what() << '\n';
        return 5;
    }
}
