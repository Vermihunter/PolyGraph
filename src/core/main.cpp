//
// Created by akos on 7. 5. 2026.
//

#include <iostream>
#include <fstream>
#include <memory>

#include <argparse/argparse.hpp>

#include "algorithms/bfs/BfsExecutor.hpp"
#include "data_access/adapters/ArangoDB/ArangoGraphStore.hpp"
#include "data_access/adapters/Memgraph/MemgraphGraphStore.hpp"
#include "data_access/adapters/Neo4J/Neo4jGraphStore.hpp"
#include "data_access/adapters/Neo4J/Neo4JQueryVisitor.hpp"
#include "domain/graph/TypeId.hpp"
#include "../../include/infrastructure/DatabaseContext.hpp"
#include "data_access/adapters/ArangoDB/ArangoQueryVisitor.hpp"
#include "data_access/adapters/cuGraph/CuGraphProjectionStore.hpp"
#include "gpu/data_model/GpuProjectionBuilder.hpp"
#include "infrastructure/BackendId.hpp"
#include "infrastructure/PlacementCatalog.hpp"
#include "utilities/BackendIds.hpp"
#include <cuda_runtime_api.h>

#include "utilities/Profiling.hpp"

void check_cuda_or_throw() {
    int device_count = 0;

    cudaError_t err = cudaGetDeviceCount(&device_count);

    if (err != cudaSuccess) {
        throw std::runtime_error(
            std::string("cudaGetDeviceCount failed: ") +
            cudaGetErrorString(err)
        );
    }

    if (device_count == 0) {
        throw std::runtime_error("No CUDA devices available");
    }

    err = cudaSetDevice(0);

    if (err != cudaSuccess) {
        throw std::runtime_error(
            std::string("cudaSetDevice failed: ") +
            cudaGetErrorString(err)
        );
    }

    /*
     * Force CUDA context creation.
     */
    err = cudaFree(nullptr);

    if (err != cudaSuccess) {
        throw std::runtime_error(
            std::string("CUDA context initialization failed: ") +
            cudaGetErrorString(err)
        );
    }
}

std::unordered_map<std::string, BackendId> strToBackendId {
    {
       {"Neo4J", NEO4J},
        {"ArangoDB", ARANGO},
        {"GPU", GPU}
    }};


ExpandSpec spec{
    .allowed_edge_types = {}, // LIKES, WROTE
    .allowed_target_node_types = {}, // POST
    .direction = Direction::Undirected
};

BfsOptions options {
    .max_depth = 7,
    .return_predecessors = true,
    .allow_native_pushdown = false,
    .allow_partial_acceleration = true
};

std::vector<domain::Property> get_properties(const json& prop) {
    std::vector<domain::Property> properties;

    std::unordered_map<std::string, domain::PropertyType> strToType {{
        {"string", domain::PropertyType::string},
        {"integer", domain::PropertyType::integer},
        {"boolean", domain::PropertyType::boolean}

    }};

    for (auto&& p : prop) {
        properties.push_back(domain::Property{
            .name = p["name"],
            .type = strToType[p["type"]]
        });
    }

    return properties;
}

struct ProjectionBuildResult {
    std::vector<record_t> gpu_records;
    std::shared_ptr<HostAdjacencyIndex> host_index;
};

inline ProjectionBuildResult build_host_and_gpu_projection_inputs(
    const std::string& filename,
    const std::shared_ptr<domain::LogicalGraphSchema>& schema
) {
    std::ifstream ifs(filename);

    if (!ifs) {
        throw std::runtime_error("Could not open file: " + filename);
    }

    json data = json::parse(ifs);

    ProjectionBuildResult result;
    result.host_index = std::make_shared<HostAdjacencyIndex>();

    std::unordered_map<GlobalId, NodeRef, boost::hash<GlobalId>> nodes_by_id;
    std::unordered_map<std::string, GlobalId> id_by_uuid;

    // ------------------------------------------------------------
    // 1. Load all nodes first.
    // ------------------------------------------------------------
    for (const auto& node_json : data["nodes"]) {
        const auto uuid_str = node_json["uuid"].get<std::string>();
        const auto type_name = node_json["type"].get<std::string>();
        const auto source = node_json["source"].get<std::string>();

        GlobalId gid = boost::lexical_cast<boost::uuids::uuid>(uuid_str);
        TypeId type_id = schema->node_type_id(type_name);

        NodeRef ref{
            .global_id = gid,
            .type = type_id
        };

        nodes_by_id.emplace(gid, ref);
        id_by_uuid.emplace(uuid_str, gid);

        if (source == "GPU") {
            auto record = std::make_unique<Record>();

            record->fields.push_back(Node{
                .global_id = gid,
                .type = type_id,
                .labels = {type_name},
                .properties = node_json.value("properties", json::object()).get<Map>()
            });

            result.gpu_records.push_back(std::move(record));
        }
    }

    // ------------------------------------------------------------
    // 2. Load relationships.
    // ------------------------------------------------------------
    for (const auto& rel_json : data["relationships"]) {
        const auto rel_uuid_str = rel_json["uuid"].get<std::string>();
        const auto rel_type_name = rel_json["type"].get<std::string>();
        const auto rel_source = rel_json["source"].get<std::string>();

        const auto from_uuid = rel_json["from"].get<std::string>();
        const auto to_uuid = rel_json["to"].get<std::string>();

        const GlobalId from_id = id_by_uuid.at(from_uuid);
        const GlobalId to_id = id_by_uuid.at(to_uuid);

        const NodeRef& from_ref = nodes_by_id.at(from_id);
        const NodeRef& to_ref = nodes_by_id.at(to_id);

        EdgeRef edge_ref{
            .global_id = boost::lexical_cast<boost::uuids::uuid>(rel_uuid_str),
            .type = schema->edge_type_id(rel_type_name),
            .from = from_ref.global_id,
            .to = to_ref.global_id
        };

        // Keep a CPU-side adjacency index for partial expansion/fallback/debugging.
        result.host_index->add_edge(from_ref, to_ref, edge_ref);

        if (rel_source == "GPU") {
            auto record = std::make_unique<Record>();

            record->fields.emplace_back(Edge{
                .global_id = edge_ref.global_id,
                .type = edge_ref.type,
                .src = from_ref.global_id,
                .dst = to_ref.global_id,
                .properties = rel_json.value("properties", json::object()).get<Map>()
            });

            result.gpu_records.push_back(std::move(record));
        }
    }

    return result;
}


std::unique_ptr<domain::LogicalGraphSchema> construct_domain_graph(
    const std::string& filename,
    std::unordered_map<BackendId, std::unordered_set<TypeId>>& nodesByBackend,
    std::unordered_map<BackendId, std::unordered_set<TypeId>>& edgesByBackend,
    std::vector<Placement>& placements
) {

    std::ifstream ifs(filename);
    if (ifs.bad()) {
        return nullptr;
    }

    json config = json::parse(ifs);

    auto graph = std::make_unique<domain::LogicalGraphSchema>();
    graph->add_node_type("Proxy", {});
    for (auto && node : config["nodeTypes"]) {
        TypeId nodeId = graph->add_node_type(node["name"], get_properties(node["properties"]));

        auto source = node["source"];
        std::cerr << node["name"] << " -> " << nodeId << " at " << source << std::endl;

        BackendId sourceBackend = strToBackendId[source];
        nodesByBackend[sourceBackend].insert(nodeId);
    }


    std::cerr << "############################" << std::endl;

    for (auto && edge : config["relations"]) {
        auto from_id = graph->node_type_id(edge["from"]);
        auto to_id = graph->node_type_id(edge["to"]);

        TypeId edgeId = graph->add_edge_type(edge["name"], from_id, to_id, true, get_properties(edge["properties"]));


        auto source = edge["source"];
        std::cerr << edge["name"] << " -> " << edgeId << " at " << source << std::endl;
        BackendId sourceBackend = strToBackendId[source];
        edgesByBackend[sourceBackend].insert(edgeId);

        placements.push_back(Placement{
            .backend_id = sourceBackend,
            .kind = sourceBackend == GPU
                ? PlacementKind::GpuProjection
                : PlacementKind::Primary,
            .source_node_types = std::unordered_set<TypeId>({from_id}),
            .edge_types = std::unordered_set<TypeId>({edgeId}),
            .target_node_types = std::unordered_set<TypeId>({to_id}),
            .authoritative = true,
            .priority = sourceBackend == GPU ? 1 : 50
        });
    }

    return graph;
}

std::vector<record_t> get_gpu_records(const std::string& filename, std::shared_ptr<domain::LogicalGraphSchema>& domain_graph) {
    std::ifstream ifs(filename);
    if (ifs.bad()) {
        return {};
    }

    json data = json::parse(ifs);

    std::vector<record_t> gpu_records;
    for (auto&& node : data["nodes"]) {
        if (auto source = node["source"].get<std::string>(); source == "GPU" ) {
            auto record = std::make_unique<Record>();

            const auto dump = node.dump();

            record->fields.push_back(Node{
                .global_id = boost::lexical_cast<boost::uuids::uuid>(node["uuid"].get<std::string>()),
                .type = domain_graph->node_type_id(node["type"]),
                .labels = {node["type"]},
                .properties = node["properties"],
            });
            gpu_records.push_back(std::move(record));
        }
    }

    return gpu_records;
}


auto build_context(const std::string& domainGraphPath, const std::string& dataPath) {


    auto ctx = DatabaseContext::create_empty();

    std::unordered_map<BackendId, std::unordered_set<TypeId>> nodesByBackend;
    std::unordered_map<BackendId, std::unordered_set<TypeId>> edgesByBackend;
    std::vector<Placement> placements;
    ctx->schema = construct_domain_graph(domainGraphPath, nodesByBackend, edgesByBackend, placements);
    ctx->current_graph_version = 1;


    for (auto& placement : placements) {
        ctx->register_placement(std::move(placement));
    }

    // -----------------------------
    // Connection configs
    // -----------------------------
    ctx->register_backend_config(BackendConnectionConfig {
        .id = NEO4J,
        .kind = BackendKind::Neo4j,
        .name = "Neo4j Primary",
        .endpoint = "http://localhost:7474/db/neo4j/tx/commit",
        .database_name = "neo4j",
        .credentials = DatabaseCredentials{
            .username = "neo4j",
            .password = "password"
        },
        .transport = "bolt",
        .bolt_endpoint = "bolt://localhost:7687"
    });

    ctx->register_backend_config(BackendConnectionConfig {
        .id = ARANGO,
        .kind = BackendKind::Arango,
        .name = "Arango University",
        .endpoint = "http://localhost:8529/_db/university/_api/cursor",
        .database_name = "university",
        .credentials = DatabaseCredentials{
            .username = "root",
            .password = "rootpassword"
        }
    });

    ctx->register_backend_config(BackendConnectionConfig {
        .id = MEMGRAPH,
        .kind = BackendKind::Memgraph,
        .name = "Memgraph Hot Cache",
        .endpoint = "bolt://localhost:7687",
        .database_name = "memgraph"
    });


    // -----------------------------
    // Backend descriptors
    // -----------------------------

    BackendDescriptor neo4j_descriptor {
        .id = NEO4J,
        .kind = BackendKind::Neo4j,
        .capabilities = BackendCapabilities{
            .supports_batch_neighbors = true,
            .supports_native_bfs = false,
            .supports_native_property_filters = true
        },
        .coverage = GraphCoverage{
            .fresh = true,
            .covers_all_node_types = false,
            .covers_all_edge_types = false,
            .complete_for_requested_subgraph = false,
            .node_types = nodesByBackend[NEO4J],
            .edge_types = edgesByBackend[NEO4J],
            .graph_version = 1
        },
        .priority = 50,
        .name = "Neo4j Primary",
    };

    /*
    BackendDescriptor memgraph_descriptor {
        .id = MEMGRAPH,
        .kind = BackendKind::Memgraph,
        .capabilities = BackendCapabilities{
            .supports_batch_neighbors = true,
            .supports_native_bfs = true,
            .supports_native_property_filters = true
        },
        .coverage = GraphCoverage{
            .fresh = true,
            .covers_all_node_types = false,
            .covers_all_edge_types = false,
            .complete_for_requested_subgraph = false,
            .node_types = nodesByBackend[MEMGRAPH],
            .edge_types = edgesByBackend[MEMGRAPH],
            .graph_version = 1
        },
        .priority = 10,
        .name = "Memgraph Hot Cache",
    };
    */

    BackendDescriptor gpu_descriptor {
        .id = GPU,
        .kind = BackendKind::CuGraph,
        .capabilities = BackendCapabilities{
            .supports_batch_neighbors = true,
            .supports_native_bfs = false,
            .supports_native_property_filters = false
        },
        .coverage = GraphCoverage{
            .fresh = true,
            .covers_all_node_types = false,
            .covers_all_edge_types = false,
            .complete_for_requested_subgraph = false,
            .node_types = nodesByBackend[GPU],
            .edge_types = edgesByBackend[GPU],
            .graph_version = 1
        },
        .priority = 1,
        .name = "GPU acceleration",
    };

    BackendDescriptor arango_descriptor {
        .id = ARANGO,
        .kind = BackendKind::Arango,

        .capabilities = BackendCapabilities{
            .supports_batch_neighbors = true,
            .supports_native_bfs = false,
            .supports_gpu_execution = false
        },
        .coverage = GraphCoverage{
            .fresh = true,
            .covers_all_node_types = false,
            .covers_all_edge_types = false,
            .complete_for_requested_subgraph = false,
            .node_types = nodesByBackend[ARANGO],
            .edge_types = edgesByBackend[ARANGO],
            .graph_version = 1
        },
        .priority = 0,
        .name = "ArangoDB Primary",
    };

    // -----------------------------
    // Register stores
    // -----------------------------
    std::shared_ptr<QueryVisitor> neo_visitor = std::make_shared<Neo4JQueryVisitor>(*ctx, ctx->connection_configs[NEO4J]);
    ctx->register_store(std::make_shared<Neo4jGraphStore>(*ctx, neo4j_descriptor, neo_visitor));

    std::shared_ptr<QueryVisitor> arango_visitor = std::make_shared<ArangoQueryVisitor>(*ctx, ctx->connection_configs[ARANGO]);
    ctx->register_store(std::make_shared<ArangoGraphStore>(*ctx, arango_descriptor, arango_visitor));
    //ctx->register_store(std::make_shared<MemgraphGraphStore>(ctx, memgraph_descriptor));

    using vertex_t = int32_t;
    using edge_t   = int32_t;
    using weight_t = float;
    using result_t = float;



    //raft::handle_t handle;
    auto build = build_host_and_gpu_projection_inputs(dataPath, ctx->schema);

    std::shared_ptr<CsrGraphProjection<vertex_t, edge_t>> gpu_projection =
        GpuProjectionBuilder::from_records<vertex_t, edge_t>(
            ctx->gpu_runtime->handle,
            build.gpu_records,
            1,
            true // mirror edges so one-hop GPU expansion can serve undirected/BOTH BFS
        );

    ctx->register_store(
        std::make_shared<CuGraphProjectionStore<CsrGraphProjection<vertex_t, edge_t>>>(
            gpu_descriptor, gpu_projection, build.host_index));

    NodeRef gpu_source_node {
        .global_id = boost::lexical_cast<boost::uuids::uuid>("c2ddf552-60fb-4a33-8baf-a41718783469"),
        .type = ctx->schema->node_type_id("Student")
    };
    //auto bfs_res = ctx->store(GPU)->run_bfs(gpu_source_node, spec, options);

    // -----------------------------
    // Placement catalog
    // -----------------------------

    /*
    ctx->register_placement(Placement {
        .backend_id = GPU,
        .kind = PlacementKind::GpuProjection,
        .source_node_types = {PERSON},
        .edge_types = {LIKES},
        .target_node_types = {POST},
        .authoritative = false,
        .priority = 0
    });

    ctx->register_placement(Placement {
        .backend_id = MEMGRAPH,
        .kind = PlacementKind::Cache,
        .source_node_types = {PERSON},
        .edge_types = {LIKES},
        .target_node_types = {POST},
        .authoritative = false,
        .priority = 10
    });

    ctx->register_placement(Placement {
        .backend_id = NEO4J,
        .kind = PlacementKind::Primary,
        .source_node_types = {PERSON},
        .edge_types = {LIKES, FOLLOWS},
        .target_node_types = {PERSON, POST},
        .authoritative = true,
        .priority = 50
    });
    */


    /*
    ctx->execution_policy = ExecutionPolicy {
        .read_consistency = ReadConsistency::EventuallyConsistent,
        .prefer_gpu = true,
        .prefer_memgraph_cache = true,
        .allow_native_backend_algorithms = true,
        .allow_distributed_frontier_execution = true,
        .min_frontier_size_for_parallelism = 512,
        .max_parallel_backend_requests = 8,
        .max_frontier_batch_size = 10'000
    };
    */

    return ctx;
}



void example_setup(argparse::ArgumentParser& args) {


    auto logicalGraphSchemaPath = args.get<std::string>("-lgs");
    auto verbose = args.get<bool>("--verbose");
    auto gpuDataPath = args.get<std::string>("--data");


    auto ctx = build_context(logicalGraphSchemaPath, gpuDataPath);
    BfsExecutor bfs(*ctx->placement_catalog);

    /*
    std::pair neo_range{
        uuid{{0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
        uuid{{0x3f,0xff,0xff,0xff,0xff,0xff,0x4f,0xff,0xbf,0xff,0xff,0xff,0xff,0xff,0xff,0xff}}
    };

    std::pair arango_range {
        uuid{{0x40,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
        uuid{{0x7f,0xff,0xff,0xff,0xff,0xff,0x4f,0xff,0xbf,0xff,0xff,0xff,0xff,0xff,0xff,0xff}}
    };
    */


    // University -> has no outgoing edges
    /*
    NodeRef source{
        .global_id = uuid({0x0f,0x26,0x56,0x4f,0x9a,0x16,0x4b,0xbe,0xb0,0x83,0x0c,0x0e,0x0b,0xde,0x56,0x15}),
        .type = ctx->schema->node_type_id("University")
    };
    */


    // 11a96cb7-c043-4aa7-aa35-2297e9be7f06
    NodeRef source {
        .global_id = uuid({0x11,0xa9,0x6c,0xb7,0xc0,0x43,0x4a,0xa7,0xaa,0x35,0x22,0x97,0xe9,0xbe,0x7f,0x06}),
        .type = ctx->schema->node_type_id("Faculty")
    };





    auto result = bfs.run(source, spec, options);

    std::cout << "vertex_id;distance\n";
    std::vector<std::pair<GlobalId, std::uint32_t>> op_vector(
        result.distance.begin(), result.distance.end());

    std::sort(op_vector.begin(), op_vector.end(),
        [](const auto& left, const auto& right) {
            return (left.second != right.second)
                ? left.second < right.second
                : boost::uuids::to_string(left.first) < boost::uuids::to_string(right.first);
        });

    for (const auto& [vertex_id, distance] : op_vector) {
        std::cout << vertex_id << ";" << distance << "\n";
    }
}

int main(int argc, char ** argv) {
    check_cuda_or_throw();
    curl_global_init(CURL_GLOBAL_ALL);

    argparse::ArgumentParser program("graph_db");

    program.add_argument("-lgs", "--logical-graph-schema")
        .help("Path to the logical path schema json file")
        .required();

    program.add_argument("--data")
        .help("Path to the JSON data file that contains the GPU nodes")
        .required();

    /*
    program.add_argument("-o", "--output")
        .help("output file")
        .default_value(std::string{"out.txt"});

    program.add_argument("-r", "--repetitions")
        .help("number of repetitions")
        .default_value(1)
        .scan<'i', int>();
    */

    program.add_argument("-v", "--verbose")
        .help("enable verbose output")
        .default_value(false)
        .implicit_value(true);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << "\n";
        std::cerr << program;
        return 1;
    }
    CPGDB_PROFILE_FUNCTION();

    try {
        // your current code
        example_setup(program);
        cpgdb::prof::Profiler::dump(std::cerr);
    } catch (...) {
        cpgdb::prof::Profiler::dump(std::cerr);
        throw;
    }


}