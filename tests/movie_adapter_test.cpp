#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>
#include "MovieAdapter.hpp"

using namespace polygraph::ingestion;
TEST(MovieAdapterTest, StreamsTypedCsvRecordsWithReusedIds) {
    InputAdapterRegistry registry;
    registry.add(polygraph::examples::movie_graph::input_adapter());
    const auto input = fs::path(POLYGRAPH_PROJECT_ROOT) / "examples/movie-graph/csv";
    auto adapter = registry.create(PrepareOptions{.adapter = "movie-graph", .input = input}, {});
    std::vector<Record> nodes, edges;
    adapter->nodes([&](Record record) { nodes.push_back(std::move(record)); });
    adapter->relationships([&](Record record) { edges.push_back(std::move(record)); });
    ASSERT_EQ(nodes.size(), 9);
    ASSERT_EQ(edges.size(), 9);
    EXPECT_EQ(nodes[0].type, "Person");
    ASSERT_TRUE(nodes[0].identity.has_value());
    ASSERT_TRUE(nodes[4].identity.has_value());
    ASSERT_TRUE(nodes[7].identity.has_value());
    EXPECT_EQ(nodes[0].identity->value, Json(std::int64_t{1}));
    EXPECT_TRUE(nodes[0].identity->value.is_number_integer());
    EXPECT_FALSE(nodes[0].identity->value.is_number_unsigned());
    EXPECT_EQ(nodes[0].properties["name"], "Ada \"A\" Vale");
    EXPECT_TRUE(nodes[0].properties["active"].is_boolean());
    EXPECT_TRUE(nodes[0].properties["birth_year"].is_number_integer());
    EXPECT_EQ(nodes[4].identity->value, nodes[0].identity->value);
    EXPECT_EQ(nodes[7].identity->value, nodes[0].identity->value);
    EXPECT_NE(source_identity_key(*nodes[0].identity), source_identity_key(*nodes[4].identity));
    EXPECT_NE(source_identity_key(*nodes[0].identity), source_identity_key(*nodes[7].identity));
    EXPECT_TRUE(nodes[4].properties["rating"].is_number_float());
    EXPECT_EQ(edges[0].from_identity->type, "Person");
    EXPECT_EQ(edges[0].to_identity->type, "Film");
    EXPECT_EQ(edges[0].properties["role"], "Captain, Navigator");
    EXPECT_TRUE(edges[0].properties["credited"].is_boolean());
    EXPECT_EQ(edges[0].identity->value, edges[4].identity->value);
    EXPECT_NE(source_identity_key(*edges[0].identity, true), source_identity_key(*edges[4].identity, true));
}

namespace {
    class MovieCsvTest : public ::testing::Test {
    protected:
        fs::path root;
        InputAdapterRegistry registry;
        void SetUp() override {
            auto pattern = (fs::temp_directory_path() / "polygraph-movie-csv-XXXXXX").string();
            const auto path = ::mkdtemp(pattern.data());
            ASSERT_NE(path, nullptr);
            root = path;
            fs::copy(fs::path(POLYGRAPH_PROJECT_ROOT) / "examples/movie-graph/csv", root / "csv",
                     fs::copy_options::recursive);
            registry.add(polygraph::examples::movie_graph::input_adapter());
        }
        void TearDown() override { fs::remove_all(root); }
        auto adapter() {
            return registry.create(PrepareOptions{.adapter = "movie-graph", .input = root / "csv"}, root / "scratch");
        }
        void write(const char *name, const std::string &data) { std::ofstream(root / "csv" / name) << data; }
    };

    TEST_F(MovieCsvTest, SupportsCrLfEscapedQuotesAndQuotedNewlines) {
        write("people.csv", "id,name,birth_year,active\r\n1,\"Ada\n\"\"A\"\"\",1970,true\r\n");
        std::vector<Record> records;
        adapter()->nodes([&](Record record) { records.push_back(std::move(record)); });
        ASSERT_FALSE(records.empty());
        EXPECT_EQ(records[0].properties["name"], "Ada\n\"A\"");
        EXPECT_FALSE(fs::exists(root / "scratch"));
    }

    TEST_F(MovieCsvTest, RejectsMalformedCsvNumbersAndHeadersWithSourceContext) {
        for (const auto &data:
             {"id,title,release_year,rating\n1,Title,2001,NaN\n", "id,title,release_year,rating\n1,Title,year,8.0\n",
              "id,title,release_year,rating\n1,\"unterminated,2001,8.0\n",
              "id,title,release_year,rating\n1,Title,2001\n", "id,title,rating,release_year\n1,Title,8.0,2001\n"}) {
            write("films.csv", data);
            try {
                adapter()->nodes([](Record) {});
                FAIL() << data;
            } catch (const Error &error) {
                EXPECT_NE(std::string(error.what()).find("films.csv:record"), std::string::npos) << error.what();
            }
        }
    }

    TEST_F(MovieCsvTest, ConsumerCancellationIsSynchronousAndUnchanged) {
        std::size_t emitted = 0;
        try {
            adapter()->nodes([&](Record) {
                ++emitted;
                throw std::logic_error("consumer stop");
            });
            FAIL();
        } catch (const std::logic_error &error) {
            EXPECT_EQ(std::string(error.what()), "consumer stop");
        }
        EXPECT_EQ(emitted, 1);
    }
} // namespace
