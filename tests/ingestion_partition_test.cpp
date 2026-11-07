#include "Internal.hpp"
#include "ingestion/Import.hpp"
#include "ingestion/Partition.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <numeric>
#include <unistd.h>

using namespace polygraph::ingestion;

namespace {
    GraphTopology path_and_isolate() { return {{0, 1, 3, 4, 4}, {1, 0, 2, 1}}; }

    GraphTopology cliques() {
        std::vector<std::vector<std::uint32_t>> rows(12);
        for (const unsigned base: {0U, 6U})
            for (unsigned u = base; u < base + 6; ++u)
                for (unsigned v = u + 1; v < base + 6; ++v) {
                    rows[u].push_back(v);
                    rows[v].push_back(u);
                }
        rows[5].push_back(6);
        rows[6].push_back(5);
        GraphTopology graph{{0}, {}};
        for (auto &row: rows) {
            std::ranges::sort(row);
            graph.neighbors.insert(graph.neighbors.end(), row.begin(), row.end());
            graph.offsets.push_back(graph.neighbors.size());
        }
        return graph;
    }
} // namespace

TEST(IngestionPartitionTest, ValidatesSimpleSymmetricCsrAndCountsUndirectedCut) {
    const auto graph = path_and_isolate();
    EXPECT_NO_THROW(validate_topology(graph));
    const std::vector<std::uint32_t> assignment{0, 0, 1, 1};
    EXPECT_EQ(topology_edge_cut(graph, assignment), 1);
    EXPECT_THROW((void) topology_edge_cut(graph, std::vector<std::uint32_t>{0}), Error);
}

TEST(IngestionPartitionTest, RejectsMalformedOffsetsNeighborsLoopsAndAsymmetry) {
    for (const GraphTopology graph: std::vector<GraphTopology>{{{}, {}},
                                                               {{0}, {}},
                                                               {{1, 1}, {}},
                                                               {{0, 2, 1}, {1}},
                                                               {{0, 1}, {1}},
                                                               {{0, 1}, {0}},
                                                               {{0, 1, 1}, {1}},
                                                               {{0, 2, 4}, {1, 1, 0, 0}},
                                                               {{0, 2, 3, 4}, {2, 1, 0, 0}}}) {
        EXPECT_THROW(validate_topology(graph), Error);
    }
}

TEST(IngestionPartitionTest, ValidatesSettingsAndSeedWithoutOptionalDependency) {
    PartitionOptions options{.count = 2};
    EXPECT_NO_THROW(validate_partition_options(options, 42));
    for (const unsigned count: {0U, 1U, std::numeric_limits<unsigned>::max()}) {
        options.count = count;
        EXPECT_THROW(validate_partition_options(options, 42), Error);
    }
    options.count = 2;
    for (const unsigned threads: {0U, 257U}) {
        options.threads = threads;
        EXPECT_THROW(validate_partition_options(options, 42), Error);
    }
    options.threads = 1;
    for (const double imbalance:
         {-0.01, 1.01, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        options.imbalance = imbalance;
        EXPECT_THROW(validate_partition_options(options, 42), Error);
    }
    options.imbalance = 0.03;
    options.preset = "unknown";
    EXPECT_THROW(validate_partition_options(options, 42), Error);
    options.preset = "default";
    EXPECT_THROW(validate_partition_options(options, std::numeric_limits<unsigned>::max()), Error);
}

#if POLYGRAPH_TEST_HAS_KAMINPAR
TEST(IngestionPartitionTest, DependencyIdentityIncludesActualLibraryModuleContents) {
    const auto identity = kaminpar_identity();
    for (const std::string field: {";module-sha256:", ";tbb-sha256:"}) {
        const auto offset = identity.find(field);
        ASSERT_NE(offset, std::string::npos) << identity;
        const auto digest = identity.substr(offset + field.size(), 64);
        ASSERT_EQ(digest.size(), 64);
        EXPECT_TRUE(
                std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    }
}

class IngestionPartitionPipelineTest : public ::testing::Test {
protected:
    fs::path directory;
    void SetUp() override {
        auto pattern = (fs::temp_directory_path() / "polygraph-partition-XXXXXX").string();
        const auto path = ::mkdtemp(pattern.data());
        ASSERT_NE(path, nullptr);
        directory = path;
    }
    void TearDown() override { fs::remove_all(directory); }
    PrepareOptions fixture() {
        const auto placement = directory / "placement.json";
        write_json_atomic(placement,
                          {{"storageBackends", {"Neo4J", "ArangoDB", "PostgreSQL"}},
                           {"nodeSources", {{"Person", "Neo4J"}, {"Post", "ArangoDB"}, {"Place", "PostgreSQL"}}},
                           {"relationshipSources", {{"ARC", "PostgreSQL"}}}});
        const auto input = directory / "graph.jsonl";
        std::ofstream stream(input);
        stream << Json{{"kind", "metadata"}, {"idRouting", {"Neo4J", "ArangoDB", "PostgreSQL"}}}.dump() << '\n';
        const std::array homes{Backend::neo4j, Backend::arango, Backend::postgresql};
        const std::array names{"Person", "Post", "Place"};
        std::vector<Id> ids;
        for (unsigned i = 0; i < 6; ++i) {
            auto id = make_id(homes[i % 3], 11 + i);
            if (i == 0)
                id |= 8ULL << 48; // Retain optional residency separately from physical identity.
            ids.push_back(id);
            stream << Json{{"kind", "node"},
                           {"uuid", signed_id(id)},
                           {"type", names[i % 3]},
                           {"properties", {{"name", "Ž|😀"}, {"tags", {"a", "b"}}}}}
                              .dump()
                   << '\n';
        }
        unsigned sequence = 0;
        for (const auto [from, to]: std::vector<std::pair<unsigned, unsigned>>{
                     {0, 1}, {1, 2}, {2, 0}, {3, 4}, {4, 5}, {5, 3}, {2, 3}, {1, 0}, {0, 1}, {5, 5}})
            stream << Json{{"kind", "relationship"},
                           {"uuid", signed_id(make_id(Backend::postgresql, (1ULL << 47) | sequence++))},
                           {"type", "ARC"},
                           {"from", signed_id(ids[from])},
                           {"to", signed_id(ids[to])},
                           {"properties", {{"when", "2026-11-05"}}}}
                              .dump()
                   << '\n';
        stream.close();
        return {.adapter = "polygraph-jsonl",
                .input = input,
                .placement_config = placement,
                .output_dir = directory / "none",
                .memory_limit_mb = 32,
                .chunk_size_mb = 1,
                .max_record_mb = 4};
    }
    Json graph_identity(const fs::path &root, const Json &manifest) {
        detail::Database db(root / manifest["index"]["path"].get<std::string>(), 1, true);
        Json identity = Json::object();
        for (const auto &[table, query]: std::vector<std::pair<std::string, std::string>>{
                     {"nodes", "SELECT id,original,key,type,owner,residency,properties FROM nodes ORDER BY id"},
                     {"edges", "SELECT id,original,type,owner,from_id,to_id,properties FROM edges ORDER BY id"}}) {
            auto rows = db.statement(query);
            identity[table] = Json::array();
            while (rows.step()) {
                auto properties = Json::parse(rows.text(6));
                for (const auto *key: {"partition_id", "from_partition_id", "to_partition_id", "is_partition_cut"})
                    properties.erase(key);
                if (table == "nodes")
                    identity[table].push_back(Json::array({rows.integer(0), rows.integer(1), rows.text(2), rows.text(3),
                                                           rows.text(4), rows.integer(5), properties}));
                else
                    identity[table].push_back(Json::array({rows.integer(0), rows.integer(1), rows.text(2), rows.text(3),
                                                           rows.integer(4), rows.integer(5), properties}));
            }
        }
        for (const auto *table: {"proxies", "replicas"}) {
            auto rows = db.statement("SELECT * FROM " + std::string(table) + " ORDER BY 1,2");
            identity[table] = Json::array();
            while (rows.step())
                identity[table].push_back(Json::array({rows.text(0), rows.integer(1)}));
        }
        return identity;
    }
};

TEST_F(IngestionPartitionPipelineTest, GeneratedPartitionsPreserveCanonicalGraphAndProjectDuplicatesAndLoops) {
    auto options = fixture();
    const auto baseline = prepare(options);
    const auto expected = graph_identity(options.output_dir, baseline);
    options.output_dir = directory / "partitioned";
    options.partitioner = "kaminpar";
    options.partition_options.count = 2;
    const auto partitioned = prepare(options);
    EXPECT_EQ(graph_identity(options.output_dir, partitioned), expected);
    const auto &metrics = partitioned["partitioning"];
    EXPECT_EQ(metrics["vertices"], 6);
    EXPECT_EQ(metrics["input_relationships"], 10);
    EXPECT_EQ(metrics["undirected_edges"], 7);
    EXPECT_EQ(metrics["ignored_self_loops"], 1);
    EXPECT_EQ(metrics["duplicates_collapsed"], 2);
    EXPECT_EQ(metrics["edge_cut"], 1);
    EXPECT_EQ(metrics["balanced"], true);
    EXPECT_NO_THROW((void) verify(options.output_dir / "manifest.json", true));
    EXPECT_EQ(prepare(options)["generation_id"], partitioned["generation_id"]);
    options.seed = 43;
    EXPECT_THROW((void) prepare(options), Error);
}

TEST_F(IngestionPartitionPipelineTest, GeneratedMappingCannotRewriteAlreadyCanonicalOwnership) {
    auto options = fixture();
    options.partitioner = "kaminpar";
    options.partition_options.count = 2;
    options.partition_map = directory / "map.json";
    write_json_atomic(options.partition_map, {{"0", "Neo4J"}, {"1", "ArangoDB"}});
    EXPECT_THROW((void) prepare(options), Error);
    EXPECT_FALSE(fs::exists(options.output_dir / "manifest.json"));
}

TEST_F(IngestionPartitionPipelineTest, MissingEndpointAndIncompleteMapFailBeforePublication) {
    auto options = fixture();
    options.partitioner = "kaminpar";
    options.partition_options.count = 2;
    {
        std::ofstream stream(options.input, std::ios::app);
        stream << Json{{"kind", "relationship"},
                       {"type", "ARC"},
                       {"uuid", make_id(Backend::postgresql, (1ULL << 47) | 99)},
                       {"from", make_id(Backend::neo4j, 999)},
                       {"to", make_id(Backend::neo4j, 11)}}
                          .dump()
               << '\n';
    }
    EXPECT_THROW((void) prepare(options), Error);
    EXPECT_FALSE(fs::exists(options.output_dir / "manifest.json"));
    options.output_dir = directory / "incomplete-map";
    options.partition_map = directory / "incomplete.json";
    write_json_atomic(options.partition_map, {{"0", "Neo4J"}});
    try {
        (void) prepare(options);
        FAIL() << "Incomplete partition map was accepted";
    } catch (const Error &error) {
        EXPECT_NE(std::string(error.what()).find("no backend mapping: 1"), std::string::npos);
    }
}

TEST_F(IngestionPartitionPipelineTest, RejectsOversizedTopologyBuffersBeforeAllocation) {
    auto options = fixture();
    options.partitioner = "kaminpar";
    options.partition_options.count = 2;
    options.memory_limit_mb = 16;
    {
        std::ofstream stream(options.input);
        stream << Json{{"kind", "metadata"}, {"idRouting", {"Neo4J", "ArangoDB", "PostgreSQL"}}}.dump() << '\n';
        for (unsigned i = 0; i < 270000; ++i)
            stream << Json{{"kind", "node"}, {"type", "Person"}, {"uuid", make_id(Backend::neo4j, i)}}.dump() << '\n';
    }
    try {
        (void) prepare(options);
        FAIL() << "Oversized CSR buffers were accepted";
    } catch (const Error &error) {
        EXPECT_NE(std::string(error.what()).find("buffers exceed --memory-limit-mb"), std::string::npos);
    }
    EXPECT_FALSE(fs::exists(options.output_dir / "manifest.json"));
}

TEST(IngestionPartitionTest, RealKaMinParFindsBridgeAndRepeatsWithSequentialSeed) {
    ASSERT_TRUE(kaminpar_available());
    const auto graph = cliques();
    const PartitionOptions options{.count = 2};
    const auto first = partition_topology(graph, options, 42);
    const auto second = partition_topology(graph, options, 42);
    EXPECT_EQ(first.assignment, second.assignment);
    EXPECT_EQ(first.edge_cut, 1);
    EXPECT_EQ(first.populations, (std::vector<std::uint64_t>{6, 6}));
    for (unsigned i = 0; i < 6; ++i) {
        EXPECT_EQ(first.assignment[i], first.assignment[0]);
        EXPECT_EQ(first.assignment[i + 6], first.assignment[6]);
    }
    EXPECT_NE(first.assignment[0], first.assignment[6]);
    EXPECT_TRUE(first.balanced);
    EXPECT_FALSE(kaminpar_identity().empty());
}

TEST(IngestionPartitionTest, RealKaMinParHandlesIsolatesAndCopiesInputBuffers) {
    const GraphTopology graph{{0, 0, 0, 0, 0, 0, 0}, {}};
    const auto result = partition_topology(graph, {.count = 3, .threads = 2}, 7);
    EXPECT_EQ(result.assignment.size(), 6);
    EXPECT_EQ(result.populations.size(), 3);
    EXPECT_EQ(std::accumulate(result.populations.begin(), result.populations.end(), 0ULL), 6);
    EXPECT_EQ(result.edge_cut, 0);
    EXPECT_TRUE(result.balanced);
    EXPECT_EQ(graph.offsets, (std::vector<std::uint64_t>{0, 0, 0, 0, 0, 0, 0}));
    EXPECT_TRUE(graph.neighbors.empty());
    EXPECT_THROW((void) partition_topology(graph, {.count = 7}, 7), Error);
}

TEST(IngestionPartitionTest, RealKaMinParSupportsAdvertisedPresets) {
    const auto graph = cliques();
    for (const auto *preset: {"default", "eco", "strong"}) {
        const auto result = partition_topology(graph, {.count = 2, .preset = preset}, 42);
        EXPECT_EQ(result.edge_cut, 1) << preset;
        EXPECT_TRUE(result.balanced) << preset;
    }
}
#else
TEST(IngestionPartitionTest, UnavailableKaMinParRejectsRatherThanSubstituting) {
    EXPECT_FALSE(kaminpar_available());
    EXPECT_THROW((void) partition_topology(path_and_isolate(), {.count = 2}, 42), Error);
}
#endif
