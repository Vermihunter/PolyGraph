#include "ingestion/InputAdapterRegistry.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <unistd.h>

using namespace polygraph::ingestion;

namespace {
    class GeneratedAdapter final : public IGraphInputAdapter {
    public:
        GeneratedAdapter(std::size_t count, std::size_t &emitted, bool fail_edges = false) :
            count_(count), emitted_(emitted), fail_edges_(fail_edges) {}
        void nodes(const RecordConsumer &consume) override {
            constexpr std::array homes{Backend::neo4j, Backend::arango, Backend::postgresql};
            for (std::size_t i = 0; i < count_; ++i) {
                ++emitted_;
                consume(Record{.key = "node:" + std::to_string(i),
                               .type = "Device",
                               .properties = {{"name", "device"}},
                               .id = make_id(homes[i % homes.size()], i + 1)});
            }
        }
        void relationships(const RecordConsumer &consume) override {
            for (std::size_t i = 1; i < count_; ++i) {
                if (fail_edges_)
                    throw Error("generated.rows:4: malformed endpoint");
                consume(Record{.type = "LINK",
                               .id = make_id(Backend::neo4j, count_ + i),
                               .from = "node:" + std::to_string(i - 1),
                               .to = "node:" + std::to_string(i)});
            }
        }
        Json metadata() const override { return {{"idRouting", {"Neo4J", "ArangoDB", "PostgreSQL"}}}; }

    private:
        std::size_t count_;
        std::size_t &emitted_;
        bool fail_edges_;
    };

    InputAdapterDefinition generated(std::size_t &emitted, std::size_t count = 3, bool fail_edges = false) {
        return {.name = "test-generated",
                .factory =
                        [&emitted, count, fail_edges](const PrepareOptions &, const fs::path &) {
                            return std::make_unique<GeneratedAdapter>(count, emitted, fail_edges);
                        },
                .input_files = [](const PrepareOptions &o) { return std::vector<fs::path>{o.input}; },
                .version = "1"};
    }

    TEST(InputAdapterRegistryTest, DiscoversBuiltinsAndReportsUnknownNameBeforeFactoryOrInventory) {
        const auto &registry = default_input_adapters();
        EXPECT_EQ(registry.names(), (std::vector<std::string>{"ldbc-snb", "polygraph-jsonl", "source-jsonl"}));
        try {
            (void) registry.create(PrepareOptions{.adapter = "missing"}, "missing-scratch");
            FAIL() << "Unknown adapter was accepted";
        } catch (const Error &e) {
            EXPECT_EQ(std::string(e.what()),
                      "Unknown adapter: missing (available: ldbc-snb, polygraph-jsonl, source-jsonl)");
        }
    }

    TEST(InputAdapterRegistryTest, RejectsDuplicateInvalidAndIncompleteRegistrationAndNullFactoryResult) {
        std::size_t emitted = 0;
        InputAdapterRegistry registry;
        registry.add(generated(emitted));
        EXPECT_THROW(registry.add(generated(emitted)), Error);
        auto definition = generated(emitted);
        definition.name = "invalid name";
        EXPECT_THROW(registry.add(definition), Error);
        definition.name = "no-factory";
        definition.factory = {};
        EXPECT_THROW(registry.add(definition), Error);
        definition = generated(emitted);
        definition.name = "no-inventory";
        definition.input_files = {};
        EXPECT_THROW(registry.add(definition), Error);
        definition = generated(emitted);
        definition.name = "unversioned";
        definition.version.clear();
        EXPECT_THROW(registry.add(definition), Error);
        definition = generated(emitted);
        definition.name = "null";
        definition.factory = [](const PrepareOptions &, const fs::path &) {
            return std::unique_ptr<IGraphInputAdapter>{};
        };
        registry.add(definition);
        EXPECT_THROW((void) registry.create(PrepareOptions{.adapter = "null"}, {}), Error);
    }

    TEST(InputAdapterRegistryTest, StreamsSynchronouslyAndStopsImmediatelyOnConsumerError) {
        std::size_t emitted = 0;
        InputAdapterRegistry registry;
        registry.add(generated(emitted, 100000));
        auto adapter = registry.create(PrepareOptions{.adapter = "test-generated"}, {});
        EXPECT_EQ(emitted, 0);
        std::size_t received = 0;
        EXPECT_THROW(adapter->nodes([&](Record record) {
            EXPECT_EQ(record.key, "node:" + std::to_string(received));
            EXPECT_EQ(emitted, ++received);
            if (received == 10)
                throw std::runtime_error("consumer cancelled");
        }),
                     std::runtime_error);
        EXPECT_EQ(emitted, 10);
        EXPECT_EQ(received, 10);
    }

    TEST(InputAdapterRegistryTest, FactoryErrorsPreserveSourceContext) {
        std::size_t emitted = 0;
        InputAdapterRegistry registry;
        auto definition = generated(emitted);
        definition.factory = [](const PrepareOptions &, const fs::path &) -> std::unique_ptr<IGraphInputAdapter> {
            throw Error("source.rows: missing header");
        };
        registry.add(definition);
        try {
            (void) registry.create(PrepareOptions{.adapter = "test-generated"}, {});
            FAIL() << "Factory error was swallowed";
        } catch (const Error &e) {
            EXPECT_EQ(std::string(e.what()), "source.rows: missing header");
        }
    }

    class InputAdapterPipelineTest : public ::testing::Test {
    protected:
        fs::path directory;
        std::size_t emitted = 0;
        void SetUp() override {
            auto pattern = (fs::temp_directory_path() / "polygraph-adapter-XXXXXX").string();
            const auto path = ::mkdtemp(pattern.data());
            ASSERT_NE(path, nullptr);
            directory = path;
            std::ofstream(directory / "source.rows") << "immutable source\n";
            write_json_atomic(directory / "placement.json", {{"storageBackends", {"Neo4J", "ArangoDB", "PostgreSQL"}}});
        }
        void TearDown() override { fs::remove_all(directory); }
        PrepareOptions options(const InputAdapterRegistry &registry) const {
            return {.adapter = "test-generated",
                    .input = directory / "source.rows",
                    .placement_config = directory / "placement.json",
                    .output_dir = directory / "out",
                    .input_adapters = &registry};
        }
    };

    TEST_F(InputAdapterPipelineTest, SourceJsonlRetainsExactTypedIdentitiesAndEndpointClosure) {
        const auto identity = [](Json id, const std::string &kind) {
            return Json{{"namespace", "app"}, {"type", "Device"}, {"idType", kind}, {"id", id}};
        };
        const auto signed_key = identity(7, "int64"), string_key = identity("7", "string");
        std::ofstream source(directory / "source.rows");
        source << Json{{"kind", "node"}, {"type", "Device"}, {"identity", signed_key}, {"partition", 9}}.dump() << '\n';
        source << Json{{"kind", "node"}, {"type", "Device"}, {"identity", string_key}}.dump() << '\n';
        source << Json{{"kind", "relationship"},
                       {"type", "LINK"},
                       {"identity", {{"namespace", "app"}, {"type", "LINK"}, {"idType", "string"}, {"id", "edge"}}},
                       {"from", signed_key},
                       {"to", string_key}}
                          .dump()
               << '\n';
        source.close();
        write_json_atomic(directory / "placement.json", {{"storageBackends", {"PostgreSQL"}},
                                                         {"defaultNodeSource", "PostgreSQL"},
                                                         {"defaultRelationshipSource", "PostgreSQL"}});
        auto o = options(default_input_adapters());
        o.adapter = "source-jsonl";
        auto adapter = default_input_adapters().create(o, directory);
        unsigned seen = 0;
        adapter->nodes([&](Record r) {
            if (seen++ == 0)
                EXPECT_EQ(r.partition, std::optional<unsigned>{9});
        });
        const auto manifest = prepare(o);
        EXPECT_EQ(manifest["id_strategy"], "typed-external-v1");
        EXPECT_EQ(manifest["counts"]["nodes"], 2);
        EXPECT_EQ(manifest["counts"]["edges"], 1);
        EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
    }

    TEST_F(InputAdapterPipelineTest, SourceJsonlRejectsAmbiguousCanonicalAndInvalidSourceIdsWithLineContext) {
        for (const auto &record:
             {Json{{"type", "Device"}}, Json{{"kind", "node"}, {"type", "Device"}, {"uuid", 7}},
              Json{{"kind", "node"},
                   {"type", "Device"},
                   {"identity",
                    {{"namespace", "app"}, {"type", "Device"}, {"idType", "int64"}, {"id", UINT64_MAX}}}}}) {
            std::ofstream(directory / "source.rows") << record.dump() << '\n';
            auto o = options(default_input_adapters());
            o.adapter = "source-jsonl";
            auto adapter = default_input_adapters().create(o, directory);
            try {
                adapter->nodes([](Record) {});
                FAIL() << "Invalid source JSONL accepted";
            } catch (const Error &e) {
                EXPECT_NE(std::string(e.what()).find("source.rows:1:"), std::string::npos);
            }
        }
    }

    TEST_F(InputAdapterPipelineTest, RegisteredAdapterUsesAllExistingWritersAndStrictRecovery) {
        auto registry = default_input_adapters();
        registry.add(generated(emitted));
        const auto o = options(registry);
        const auto manifest = prepare(o);
        EXPECT_EQ(manifest["counts"], (Json{{"nodes", 3}, {"edges", 2}}));
        EXPECT_EQ(manifest["id_strategy"], "preserve-canonical-v2");
        EXPECT_EQ(manifest["source"]["adapter_version"], "1");
        for (const auto &backend: {"Neo4J", "ArangoDB", "PostgreSQL"}) {
            EXPECT_EQ(manifest["backends"][backend]["counts"]["nodes"], 1);
            EXPECT_GT(manifest["backends"][backend]["counts"]["edges"].get<unsigned>(), 0);
        }
        EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
        EXPECT_EQ(prepare(o)["generation_id"], manifest["generation_id"]);
        EXPECT_EQ(emitted, 3); // Recovery must not invoke a second stream.
        std::ofstream(o.input, std::ios::app) << "changed\n";
        EXPECT_THROW((void) prepare(o), Error);
    }

    TEST_F(InputAdapterPipelineTest, AdapterVersionChangeInvalidatesExistingGeneration) {
        InputAdapterRegistry registry;
        registry.add(generated(emitted));
        (void) prepare(options(registry));
        InputAdapterRegistry updated;
        auto definition = generated(emitted);
        definition.version = "2";
        updated.add(definition);
        EXPECT_THROW((void) prepare(options(updated)), Error);
    }

    TEST_F(InputAdapterPipelineTest, IdPolicyChangeInvalidatesExistingGenerationEvenWithSameVersion) {
        InputAdapterRegistry registry;
        registry.add(generated(emitted));
        (void) prepare(options(registry));
        InputAdapterRegistry updated;
        auto definition = generated(emitted);
        definition.id_policy = InputIdPolicy::ldbc_block31;
        updated.add(definition);
        EXPECT_THROW((void) prepare(options(updated)), Error);
    }

    TEST_F(InputAdapterPipelineTest, ParsingErrorPropagatesWithSourceContextAndNoManifest) {
        InputAdapterRegistry registry;
        registry.add(generated(emitted, 3, true));
        const auto o = options(registry);
        try {
            (void) prepare(o);
            FAIL() << "Malformed adapter input was accepted";
        } catch (const Error &e) {
            EXPECT_EQ(std::string(e.what()), "generated.rows:4: malformed endpoint");
        }
        EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
    }

    TEST_F(InputAdapterPipelineTest, InventoryMutationDuringStreamingInvalidatesCheckpoint) {
        InputAdapterRegistry registry;
        registry.add(generated(emitted));
        const auto o = options(registry);
        EXPECT_THROW((void) prepare(o,
                                    [&](const std::string &stage) {
                                        if (stage == "source-hashed")
                                            std::ofstream(o.input, std::ios::app) << "changed during prepare\n";
                                    }),
                     Error);
        EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
        EXPECT_EQ(read_json(o.output_dir / "generation-state.json")["invalidated"], true);
    }

    TEST_F(InputAdapterPipelineTest, CanonicalAdapterDoesNotFallBackToLdbcIds) {
        class IdlessAdapter final : public IGraphInputAdapter {
        public:
            void nodes(const RecordConsumer &consume) override {
                consume(Record{.key = "string-key", .type = "Device"});
            }
            void relationships(const RecordConsumer &) override {}
            Json metadata() const override { return Json::object(); }
        };
        InputAdapterRegistry registry;
        auto definition = generated(emitted);
        definition.factory = [](const PrepareOptions &, const fs::path &) { return std::make_unique<IdlessAdapter>(); };
        registry.add(definition);
        const auto o = options(registry);
        try {
            (void) prepare(o);
            FAIL() << "ID-less canonical record was accepted";
        } catch (const Error &e) {
            EXPECT_EQ(std::string(e.what()), "Adapter test-generated requires canonical GlobalId on node string-key");
        }
        EXPECT_FALSE(fs::exists(o.output_dir / "manifest.json"));
    }

    TEST_F(InputAdapterPipelineTest, ExternalPartitionsUseRegisteredSourceKeysWithoutRewritingCanonicalOwners) {
        InputAdapterRegistry registry;
        registry.add(generated(emitted));
        auto o = options(registry);
        o.partition_file = directory / "partitions.jsonl";
        o.partition_map = directory / "partition-map.json";
        write_json_atomic(o.partition_map, {{"0", "Neo4J"}, {"1", "ArangoDB"}, {"2", "PostgreSQL"}});
        std::ofstream(o.partition_file) << "{\"key\":\"node:0\",\"partition\":0}\n"
                                           "{\"key\":\"node:1\",\"partition\":1}\n"
                                           "{\"key\":\"node:2\",\"partition\":2}\n";
        const auto manifest = prepare(o);
        EXPECT_EQ(manifest["partitioning"]["partition_cut_relationships"], 2);
        EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
    }

    TEST_F(InputAdapterPipelineTest, BuiltinsKeepLegacyIdentityShapeAndIdStrategies) {
        const auto fixture = fs::path(POLYGRAPH_PROJECT_ROOT) / "tests/fixtures/ingestion";
        for (const auto &name: {"ldbc-snb", "polygraph-jsonl"}) {
            const bool ldbc = std::string_view(name) == "ldbc-snb";
            PrepareOptions o{.adapter = name,
                             .input = fixture / (ldbc ? "raw" : "canonical.jsonl"),
                             .placement_config = fixture / "placement.json",
                             .output_dir = directory / name};
            const auto manifest = prepare(o);
            EXPECT_EQ(manifest["id_strategy"], ldbc ? "ldbc-block31-v3" : "preserve-canonical-v2");
            EXPECT_EQ(manifest["source"].size(), 3);
            EXPECT_EQ(manifest["source"]["adapter"], name);
            EXPECT_EQ(manifest["source"]["path"], fs::canonical(o.input).string());
            EXPECT_FALSE(manifest["source"].contains("adapter_version"));
            EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
        }
    }

    TEST_F(InputAdapterPipelineTest, PartitionProjectionRecreatesRegisteredAdapter) {
        if (!kaminpar_available())
            GTEST_SKIP() << "Requires optional KaMinPar build";
        InputAdapterRegistry registry;
        registry.add(generated(emitted));
        auto o = options(registry);
        o.partitioner = "kaminpar";
        o.partition_options.count = 2;
        const auto manifest = prepare(o);
        EXPECT_EQ(manifest["counts"], (Json{{"nodes", 3}, {"edges", 2}}));
        EXPECT_EQ(emitted, 6); // Topology and normalization each create a fresh adapter.
        EXPECT_NO_THROW((void) verify(o.output_dir / "manifest.json", true));
    }
} // namespace
