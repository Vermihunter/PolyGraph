#include "MovieAdapter.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace polygraph::examples::movie_graph {
    namespace {
        using namespace ingestion;
        using Row = std::vector<std::string>;
        constexpr std::array files{"people.csv",   "films.csv",    "studios.csv",
                                   "acted_in.csv", "directed.csv", "produced_by.csv"};

        // CSV dialect for this example: comma-separated UTF-8, doubled quotes, quoted
        // newlines, LF/CRLF. Parsing is bounded by the engine's per-record budget.
        class Table {
            std::ifstream stream_;
            fs::path path_;
            std::size_t limit_;
            std::size_t record_ = 0;

        public:
            Table(const fs::path &path, const Row &header, std::size_t limit) :
                stream_(path, std::ios::binary), path_(path), limit_(limit) {
                if (!stream_)
                    error("cannot open CSV file");
                Row actual;
                if (!next(actual) || actual != header)
                    error("unexpected header; expected named columns in documented order");
            }
            [[noreturn]] void error(const std::string &message) const {
                throw Error(path_.string() + ":record " + std::to_string(record_) + ": " + message);
            }
            bool next(Row &fields) {
                enum class State { start, plain, quoted, closed };
                auto state = State::start;
                fields.clear();
                std::string field;
                std::size_t bytes = 0;
                ++record_;
                const auto append_field = [&] {
                    fields.push_back(std::move(field));
                    field.clear();
                    state = State::start;
                };
                char c = 0;
                while (stream_.get(c)) {
                    if (++bytes > limit_)
                        error("CSV record exceeds max-record-mb");
                    if (c == '\0')
                        error("NUL in CSV record");
                    if (state == State::quoted) {
                        if (c == '"')
                            state = State::closed;
                        else
                            field += c;
                    } else if (state == State::closed && c == '"') {
                        field += c;
                        state = State::quoted;
                    } else if (c == ',') {
                        append_field();
                    } else if (c == '\n' || c == '\r') {
                        if (c == '\r' && stream_.peek() == '\n')
                            stream_.get(c);
                        append_field();
                        return true;
                    } else if (c == '"' && state == State::start) {
                        state = State::quoted;
                    } else {
                        if (c == '"' || state == State::closed)
                            error("invalid CSV quoting");
                        field += c;
                        state = State::plain;
                    }
                }
                if (!stream_.eof())
                    error("CSV read failed");
                if (state == State::quoted)
                    error("unterminated quoted field");
                if (bytes)
                    append_field();
                return bytes != 0;
            }
        };

        template<typename Number>
        Number number(const Row &row, std::size_t column, const std::string &name) {
            Number result{};
            const auto &text = row.at(column);
            const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), result);
            if (text.empty() || status != std::errc{} || end != text.data() + text.size())
                throw Error("invalid numeric column " + name + ": " + text);
            if constexpr (std::is_floating_point_v<Number>)
                if (!std::isfinite(result))
                    throw Error("nonfinite numeric column " + name);
            return result;
        }
        bool boolean(const Row &row, std::size_t column, const std::string &name) {
            if (row.at(column) == "true")
                return true;
            if (row.at(column) == "false")
                return false;
            throw Error("boolean column " + name + " must be true or false");
        }
        SourceIdentity identity(const std::string &type, std::int64_t value) {
            return {"movie-graph", type, Json(value)};
        }

        Record vertex(SourceIdentity id, Json properties) {
            Record record{};
            record.type = id.type;
            record.properties = std::move(properties);
            record.identity = std::move(id);
            return record;
        }
        Record relationship(SourceIdentity id, SourceIdentity from, SourceIdentity to, Json properties) {
            auto record = vertex(std::move(id), std::move(properties));
            record.from_identity = std::move(from);
            record.to_identity = std::move(to);
            return record;
        }

        class MovieAdapter final : public IGraphInputAdapter {
            fs::path input_;
            std::size_t limit_;
            template<typename MakeRecord>
            void table(const char *filename, const Row &header, const RecordConsumer &consume, MakeRecord make) const {
                Table csv(input_ / filename, header, limit_);
                Row row;
                while (csv.next(row)) {
                    Record record;
                    try {
                        if (row.size() != header.size())
                            throw Error("wrong column count");
                        record = make(row);
                    } catch (const Error &error) {
                        csv.error(error.what());
                    }
                    // Consumer errors/cancellation propagate unchanged, synchronously.
                    consume(std::move(record));
                }
            }

        public:
            explicit MovieAdapter(const PrepareOptions &options) :
                input_(options.input), limit_(static_cast<std::size_t>(options.max_record_mb) * 1024 * 1024) {}
            void nodes(const RecordConsumer &consume) override {
                table("people.csv", {"id", "name", "birth_year", "active"}, consume, [](const Row &row) {
                    return vertex(identity("Person", number<std::int64_t>(row, 0, "id")),
                                  {{"name", row[1]},
                                   {"birth_year", number<std::int64_t>(row, 2, "birth_year")},
                                   {"active", boolean(row, 3, "active")}});
                });
                table("films.csv", {"id", "title", "release_year", "rating"}, consume, [](const Row &row) {
                    return vertex(identity("Film", number<std::int64_t>(row, 0, "id")),
                                  {{"title", row[1]},
                                   {"release_year", number<std::int64_t>(row, 2, "release_year")},
                                   {"rating", number<double>(row, 3, "rating")}});
                });
                table("studios.csv", {"id", "name", "independent"}, consume, [](const Row &row) {
                    return vertex(identity("Studio", number<std::int64_t>(row, 0, "id")),
                                  {{"name", row[1]}, {"independent", boolean(row, 2, "independent")}});
                });
            }
            void relationships(const RecordConsumer &consume) override {
                table("acted_in.csv", {"id", "person_id", "film_id", "role", "credited"}, consume, [](const Row &row) {
                    return relationship(identity("ACTED_IN", number<std::int64_t>(row, 0, "id")),
                                        identity("Person", number<std::int64_t>(row, 1, "person_id")),
                                        identity("Film", number<std::int64_t>(row, 2, "film_id")),
                                        {{"role", row[3]}, {"credited", boolean(row, 4, "credited")}});
                });
                table("directed.csv", {"id", "person_id", "film_id", "fee"}, consume, [](const Row &row) {
                    return relationship(identity("DIRECTED", number<std::int64_t>(row, 0, "id")),
                                        identity("Person", number<std::int64_t>(row, 1, "person_id")),
                                        identity("Film", number<std::int64_t>(row, 2, "film_id")),
                                        {{"fee", number<std::int64_t>(row, 3, "fee")}});
                });
                table("produced_by.csv", {"id", "film_id", "studio_id", "budget"}, consume, [](const Row &row) {
                    return relationship(identity("PRODUCED_BY", number<std::int64_t>(row, 0, "id")),
                                        identity("Film", number<std::int64_t>(row, 1, "film_id")),
                                        identity("Studio", number<std::int64_t>(row, 2, "studio_id")),
                                        {{"budget", number<double>(row, 3, "budget")}});
                });
            }
            Json metadata() const override { return {{"dataset", "movie-graph"}, {"namespace", "movie-graph"}}; }
        };
    } // namespace

    ingestion::InputAdapterDefinition input_adapter() {
        return {.name = "movie-graph",
                .factory = [](const auto &options, const auto &) { return std::make_unique<MovieAdapter>(options); },
                .input_files =
                        [](const auto &options) {
                            std::vector<ingestion::fs::path> result;
                            for (const auto *name: files) {
                                const auto path = options.input / name;
                                if (!ingestion::fs::is_regular_file(path))
                                    throw ingestion::Error("movie-graph requires CSV file: " + path.string());
                                result.push_back(path);
                            }
                            return result;
                        },
                .id_policy = ingestion::InputIdPolicy::typed_external,
                .version = "1"};
    }
} // namespace polygraph::examples::movie_graph
