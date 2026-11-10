#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <regex>
#include <set>
#include "Internal.hpp"
#include "LdbcSpec.hpp"

namespace polygraph::ingestion::detail {
    namespace {
        std::vector<fs::path> snapshot_csv_paths(const fs::path &input) {
            std::vector<fs::path> paths;
            for (const auto &entry: fs::recursive_directory_iterator(input)) {
                // params.ini and updateStream.properties describe the generated dataset;
                // neither is a snapshot table or part of snapshot identity.
                if (!entry.is_regular_file() || entry.path().extension() != ".csv")
                    continue;
                auto filename = entry.path().filename().string();
                std::ranges::transform(filename, filename.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool root_level = entry.path().lexically_relative(input).parent_path().empty();
                if (root_level && (filename.starts_with("updatestream") || filename.starts_with("update_stream")))
                    continue;
                // Snapshot-directory CSVs always reach table-name and row validation,
                // even when their names resemble update streams.
                paths.push_back(entry.path());
            }
            std::ranges::sort(paths);
            return paths;
        }
        std::string trim(std::string text) {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return "";
            return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }
        std::string key(const std::string &type, const std::string &id) { return type + ":" + id; }
        Json scalar(const std::string &text, const std::string &name) {
            const auto t = trim(text);
            if (t.empty())
                return nullptr;
            if (name == "id" || name == "ldbc_id" || name == "length" || name == "classYear" || name == "workFrom" ||
                (name.size() >= 2 && (name.ends_with("id") || name.ends_with("Id") || name.ends_with("ID")))) {
                std::int64_t value = 0;
                const auto [last, error] = std::from_chars(t.data(), t.data() + t.size(), value);
                if (error == std::errc{} && last == t.data() + t.size())
                    return value;
                std::uint64_t large = 0;
                const auto [end, err] = std::from_chars(t.data(), t.data() + t.size(), large);
                if (err == std::errc{} && end == t.data() + t.size())
                    return large;
            }
            return t;
        }
        using RowConsumer = std::function<void(const std::vector<std::string> &, const std::vector<std::string> &)>;
        void rows(const fs::path &path, std::size_t max, const RowConsumer &consume) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw Error("Cannot open CSV: " + path.string());
            try {
                std::vector<std::string> header, row;
                if (!read_csv(in, header, '|', max))
                    return;
                while (!header.empty() && header.back().empty())
                    header.pop_back();
                if (header.empty())
                    throw Error("Empty CSV header");
                std::set<std::string> seen;
                static const std::regex annotation(":(?:ID|START_ID|END_ID)(?:\\([^)]*\\))?", std::regex::icase);
                for (std::size_t i = 0; i < header.size(); ++i) {
                    auto name = std::regex_replace(trim(header[i]), annotation, "");
                    if (const auto dot = name.rfind('.'); dot != std::string::npos)
                        name = name.substr(dot + 1);
                    name = trim(name);
                    if (name.empty())
                        name = "field_" + std::to_string(i);
                    // Endpoint headers are allowed to repeat, property headers are not.
                    header[i] = name;
                    if (i >= 2 && !seen.insert(name).second)
                        throw Error("Duplicate property header: " + name);
                }
                while (read_csv(in, row, '|', max)) {
                    if (std::ranges::all_of(row, [](const auto &v) { return trim(v).empty(); }))
                        continue;
                    while (row.size() > header.size() && row.back().empty())
                        row.pop_back();
                    if (row.size() > header.size())
                        throw Error("More CSV fields than header columns");
                    row.resize(header.size());
                    consume(header, row);
                }
            } catch (const Error &e) {
                throw Error(path.string() + ": " + e.what());
            }
        }
        Json properties(const std::vector<std::string> &header, const std::vector<std::string> &row, std::size_t skip) {
            Json result = Json::object();
            std::set<std::string> names;
            for (std::size_t i = skip; i < row.size(); ++i) {
                if (!names.insert(header[i]).second)
                    throw Error("Duplicate property header: " + header[i]);
                auto value = scalar(row[i], header[i]);
                if (!value.is_null())
                    result[header[i]] = std::move(value);
            }
            return result;
        }
        class JsonlAdapter final : public IGraphInputAdapter {
        public:
            explicit JsonlAdapter(const PrepareOptions &options, bool typed = false) :
                path_(options.input), max_(static_cast<std::size_t>(options.max_record_mb) * 1024 * 1024),
                typed_(typed) {}
            void nodes(const RecordConsumer &consume) override { scan(false, consume); }
            void relationships(const RecordConsumer &consume) override { scan(true, consume); }
            Json metadata() const override { return metadata_; }
            Json inspect_metadata() {
                scan(false, [](Record) {}, true);
                return metadata_;
            }

        private:
            fs::path path_;
            std::size_t max_;
            bool typed_;
            Json metadata_ = Json::object();
            void scan(bool edges, const RecordConsumer &consume, bool metadata_only = false) {
                std::ifstream in(path_, std::ios::binary);
                if (!in)
                    throw Error("Cannot open JSONL: " + path_.string());
                std::string line;
                std::size_t number = 0;
                bool records = false;
                while (in.peek() != std::char_traits<char>::eof()) {
                    line.clear();
                    char c = 0;
                    while (in.get(c) && c != '\n') {
                        if (line.size() >= max_)
                            throw Error("JSONL record exceeds --max-record-mb");
                        line += c;
                    }
                    ++number;
                    if (trim(line).empty() || trim(line).front() == '#')
                        continue;
                    try {
                        const auto value = Json::parse(line);
                        if (!value.is_object())
                            throw Error("JSONL record must be an object");
                        std::string kind = value.value("kind", std::string{});
                        std::ranges::transform(kind, kind.begin(),
                                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        if (kind.empty() && typed_)
                            throw Error("source-jsonl requires explicit record kind");
                        if (kind.empty())
                            kind = (value.contains("from") && value.contains("to"))     ? "relationship"
                                   : (value.contains("uuid") && value.contains("type")) ? "node"
                                                                                        : "metadata";
                        if (kind == "metadata") {
                            if (records)
                                throw Error("Metadata must precede records");
                            auto meta = value;
                            meta.erase("kind");
                            metadata_.update(meta);
                            if (metadata_.dump().size() > max_)
                                throw Error("Combined JSONL metadata exceeds --max-record-mb");
                            continue;
                        }
                        if (kind != "node" && kind != "relationship" && kind != "edge")
                            throw Error("Unknown JSONL record kind: " + kind);
                        if (metadata_only)
                            return;
                        records = true;
                        if (edges != (kind != "node"))
                            continue;
                        if (typed_) {
                            if (value.contains("uuid"))
                                throw Error("source-jsonl uses external identity, not canonical uuid");
                            Record r;
                            r.type = value.at("type").get<std::string>();
                            r.properties = value.value("properties", Json::object());
                            r.identity = domain::source_identity_from_json(value.at("identity"));
                            if (value.contains("partition"))
                                r.partition = value.at("partition").get<unsigned>();
                            if (value.contains("source"))
                                r.source = parse_backend(value.at("source").get<std::string>());
                            if (edges) {
                                r.from_identity = domain::source_identity_from_json(value.at("from"));
                                r.to_identity = domain::source_identity_from_json(value.at("to"));
                            }
                            consume(std::move(r));
                            continue;
                        }
                        const auto id = parse_id(value.at("uuid"));
                        Record r{.key = std::to_string(canonical_id(id)),
                                 .type = value.at("type").get<std::string>(),
                                 .properties = value.value("properties", Json::object()),
                                 .id = id,
                                 .source = {},
                                 .from = {},
                                 .to = {},
                                 .partition = {}};
                        if (value.contains("source") && value["source"] != "")
                            r.source = parse_backend(value["source"].get<std::string>());
                        if (edges) {
                            r.from = std::to_string(canonical_id(parse_id(value.at("from"))));
                            r.to = std::to_string(canonical_id(parse_id(value.at("to"))));
                        }
                        if (value.contains("partition"))
                            r.partition = value.at("partition").get<unsigned>();
                        consume(std::move(r));
                    } catch (const std::exception &e) {
                        throw Error(path_.string() + ":" + std::to_string(number) + ": " + e.what());
                    }
                }
                if (!in.eof() && in.fail())
                    throw Error("JSONL read failed");
            }
        };
        using LdbcGroups = std::map<std::string, std::vector<fs::path>>;
        LdbcGroups ldbc_groups(const fs::path &input) {
            LdbcGroups grouped;
            static const std::regex suffix("_[0-9]+_[0-9]+$");
            for (const auto &path: snapshot_csv_paths(input)) {
                const auto prefix = std::regex_replace(path.stem().string(), suffix, "");
                const bool known = prefix == "person_email_emailaddress" || prefix == "person_speaks_language" ||
                                   std::ranges::any_of(node_specs, [&](const auto &s) { return s.prefix == prefix; }) ||
                                   std::ranges::any_of(edge_specs, [&](const auto &s) { return s.prefix == prefix; });
                if (!known)
                    throw Error("Unknown LDBC CsvBasic file: " + path.string());
                grouped[prefix].push_back(path);
            }
            if (grouped.empty())
                throw Error("No LDBC CsvBasic files in " + input.string());
            return grouped;
        }
        class LdbcAdapter final : public IGraphInputAdapter {
        public:
            LdbcAdapter(const PrepareOptions &o, const fs::path &scratch) :
                max_(static_cast<std::size_t>(o.max_record_mb) * 1024 * 1024),
                aux_(scratch / "aux.sqlite3", std::max(1U, o.memory_limit_mb / 8)) {
                grouped_ = ldbc_groups(o.input);
                aux_.exec("CREATE TABLE aux(id TEXT NOT NULL,property TEXT NOT NULL,value TEXT NOT NULL); CREATE INDEX "
                          "aux_lookup ON aux(id)");
            }
            Json metadata() const override { return {{"idPayloadStrategy", "ldbc-block31-v3"}}; }
            void nodes(const RecordConsumer &consume) override {
                for (const auto &s: node_specs)
                    for (const auto &path: grouped_[std::string(s.prefix)]) {
                        if (s.type == "Person")
                            load_aux(path);
                        auto lookup = aux_.statement("SELECT property,value FROM aux WHERE id=? ORDER BY rowid");
                        rows(path, max_, [&](const auto &h, const auto &r) {
                            if (r.empty() || trim(r[0]).empty())
                                throw Error("Missing LDBC node ID");
                            Record record{.key = key(std::string(s.type), trim(r[0])),
                                          .type = std::string(s.type),
                                          .properties = properties(h, r, 1),
                                          .id = {},
                                          .source = {},
                                          .from = {},
                                          .to = {},
                                          .partition = {}};
                            record.properties["ldbc_id"] = scalar(r[0], "ldbc_id");
                            if (s.type == "Person") {
                                lookup.bind(1, trim(r[0]));
                                Json values = Json::object();
                                std::size_t bytes = 0;
                                while (lookup.step()) {
                                    auto value = lookup.text(1);
                                    bytes += value.size();
                                    if (bytes > max_)
                                        throw Error("Auxiliary values exceed --max-record-mb");
                                    const auto property = lookup.text(0);
                                    if (!values.contains(property))
                                        values[property] = Json::array();
                                    values[property].push_back(std::move(value));
                                }
                                lookup.reset();
                                record.properties.update(values);
                            }
                            consume(std::move(record));
                        });
                    }
            }
            void relationships(const RecordConsumer &consume) override {
                for (const auto &s: edge_specs)
                    for (const auto &path: grouped_[std::string(s.prefix)])
                        rows(path, max_, [&](const auto &h, const auto &r) {
                            if (r.size() < 2 || trim(r[0]).empty() || trim(r[1]).empty())
                                throw Error("Missing LDBC relationship endpoint");
                            consume(Record{.key = {},
                                           .type = std::string(s.type),
                                           .properties = properties(h, r, 2),
                                           .id = {},
                                           .source = {},
                                           .from = key(std::string(s.from_type), trim(r[0])),
                                           .to = key(std::string(s.to_type), trim(r[1])),
                                           .partition = {}});
                        });
            }

        private:
            std::size_t max_;
            Database aux_;
            std::map<std::string, std::vector<fs::path>> grouped_;
            static std::string shard(const fs::path &path) {
                static const std::regex pattern("(_[0-9]+_[0-9]+)$");
                std::smatch result;
                const auto stem = path.stem().string();
                return std::regex_search(stem, result, pattern) ? result.str() : "";
            }
            void load_aux(const fs::path &person) {
                aux_.exec("BEGIN; DELETE FROM aux");
                auto insert = aux_.statement("INSERT INTO aux VALUES(?,?,?)");
                for (const auto &[prefix, property]: std::array<std::pair<std::string, std::string>, 2>{
                             {{"person_email_emailaddress", "email"}, {"person_speaks_language", "language"}}}) {
                    const auto &candidates = grouped_[prefix];
                    const auto person_shard = shard(person);
                    const bool exact = !person_shard.empty() && std::ranges::any_of(candidates, [&](const auto &p) {
                        return shard(p) == person_shard;
                    });
                    for (const auto &path: candidates) {
                        if (exact ? shard(path) != person_shard : candidates.size() != 1 && !person_shard.empty())
                            continue;
                        rows(path, max_, [&](const auto &, const auto &r) {
                            if (r.size() < 2)
                                throw Error("Auxiliary row needs two fields");
                            if (trim(r[1]).empty())
                                return;
                            insert.bind(1, trim(r[0]));
                            insert.bind(2, property);
                            insert.bind(3, trim(r[1]));
                            insert.run();
                        });
                    }
                }
                aux_.exec("COMMIT");
            }
        };
    } // namespace
    std::unique_ptr<IGraphInputAdapter> make_adapter(const PrepareOptions &o, const fs::path &scratch) {
        return input_adapters(o).create(o, scratch);
    }
    std::vector<fs::path> input_paths(const PrepareOptions &o) {
        const auto &definition = input_adapters(o).get(o.adapter);
        auto files = definition.input_files(o);
        for (auto &path: files)
            path = fs::canonical(path);
        std::ranges::sort(files);
        if (std::adjacent_find(files.begin(), files.end()) != files.end())
            throw Error("Adapter " + o.adapter + " input inventory contains duplicate files");
        return files;
    }
    Json input_identity(const PrepareOptions &o) {
        const auto &definition = input_adapters(o).get(o.adapter);
        const auto files = input_paths(o);
        Json entries = Json::array();
        for (const auto &path: files)
            entries.push_back({{"path", path.string()}, {"bytes", fs::file_size(path)}, {"sha256", sha256_file(path)}});
        Json identity = {{"adapter", o.adapter}, {"path", fs::canonical(o.input).string()}, {"files", entries}};
        if (!definition.version.empty()) {
            identity["adapter_version"] = definition.version;
            identity["id_strategy"] = input_id_strategy(definition.id_policy);
        }
        if (!o.configuration_files.empty()) {
            auto paths = o.configuration_files;
            for (auto &path: paths)
                path = fs::canonical(path);
            std::ranges::sort(paths);
            Json configuration = Json::array();
            for (const auto &path: paths)
                configuration.push_back({{"path", path.string()}, {"sha256", sha256_file(path)}});
            identity["configuration_files"] = std::move(configuration);
        }
        return identity;
    }
} // namespace polygraph::ingestion::detail

namespace polygraph::ingestion {
    const InputAdapterRegistry &default_input_adapters() {
        static const auto registry = [] {
            InputAdapterRegistry result;
            result.add_definition(
                    {.name = "polygraph-jsonl",
                     .factory = [](const PrepareOptions &o,
                                   const fs::path &) { return std::make_unique<detail::JsonlAdapter>(o); },
                     .input_files = [](const PrepareOptions &o) { return std::vector<fs::path>{o.input}; },
                     .version = {},
                     .inspect =
                             [](const PrepareOptions &o) {
                                 detail::JsonlAdapter adapter(o);
                                 return adapter.inspect_metadata();
                             }});
            result.add_definition(
                    {.name = "source-jsonl",
                     .factory = [](const PrepareOptions &o,
                                   const fs::path &) { return std::make_unique<detail::JsonlAdapter>(o, true); },
                     .input_files = [](const PrepareOptions &o) { return std::vector<fs::path>{o.input}; },
                     .id_policy = InputIdPolicy::typed_external,
                     .version = "1"});
            result.add_definition({.name = "ldbc-snb",
                                   .factory =
                                           [](const PrepareOptions &o, const fs::path &scratch) {
                                               return std::make_unique<detail::LdbcAdapter>(o, scratch);
                                           },
                                   .input_files =
                                           [](const PrepareOptions &o) {
                                               if (!fs::is_directory(o.input))
                                                   throw Error("LDBC input must be a directory");
                                               return detail::snapshot_csv_paths(o.input);
                                           },
                                   .id_policy = InputIdPolicy::ldbc_block31,
                                   .version = {},
                                   .inspect =
                                           [](const PrepareOptions &o) {
                                               const auto groups = detail::ldbc_groups(o.input);
                                               Json nodes = Json::array(), edges = Json::array();
                                               for (const auto &spec: detail::node_specs)
                                                   if (groups.contains(std::string(spec.prefix)))
                                                       nodes.push_back(spec.type);
                                               for (const auto &spec: detail::edge_specs)
                                                   if (groups.contains(std::string(spec.prefix)))
                                                       edges.push_back(spec.type);
                                               return Json{{"nodeTypes", nodes}, {"relationshipTypes", edges}};
                                           }});
            return result;
        }();
        return registry;
    }
} // namespace polygraph::ingestion
