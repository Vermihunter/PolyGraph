#include <charconv>
#include <iostream>
#include <map>
#include <set>
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

polygraph_bulk_import verify --manifest <manifest.json> [--strict]
    Checks sizes and SHA-256; strict also checks the endpoint index and reconstructs
    native representations in bounded memory (requires temporary disk space).

polygraph_bulk_import execute --manifest <manifest.json> --target-config <targets.json>
  --confirm-create [--concurrency 3]
    Requires isolated fresh named targets. Never drops databases or resets volumes.
    All preflights must succeed before imports start. Failed imports are never retried.
    Native executors: libpq COPY, Arango HTTP import, offline neo4j-admin runner.
    Neo4j start_command and an online endpoint enable final verification.

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
        else if (command == "verify")
            allowed = {"--manifest", "--strict"};
        else if (command == "execute")
            allowed = {"--manifest", "--target-config", "--confirm-create", "--concurrency"};
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
            if (key == "--strict" || key == "--confirm-create")
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
        if (command == "prepare") {
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
                    args.flags.contains("--confirm-create"), args.number("--concurrency", 3));
            std::cout << "All native imports and live validation succeeded\n";
        } else
            std::cout << verify_live(args.require("--manifest"), args.require("--target-config")).dump(2) << '\n';
        return 0;
    } catch (const Error &e) {
        std::cerr << "Bulk ingestion: " << e.what() << '\n';
        return command == "execute" || command == "verify-live" ? 4 : 3;
    } catch (const Json::exception &e) {
        std::cerr << "Invalid configuration/record: " << e.what() << '\n';
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "Bulk ingestion I/O/internal error: " << e.what() << '\n';
        return 5;
    }
}
