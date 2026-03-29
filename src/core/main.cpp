#include <iostream>
#include <dlfcn.h>
#include <unistd.h>
#include <termios.h>
#include <vector>
#include <curl/curl.h>
#include <unordered_map>

#include <engine/Engine.hpp>
#include <query/query_types/GetAllVerticesQuery.hpp>
#include <query/query_types/GetAllEdgesQuery.hpp>

// #include <data_access/IDatabase.hpp>
// #include <data_access/adapters/JanusGraph/janus.hpp>
#include <data_access/adapters/ArangoDB/ArangoQueryVisitor.hpp>
#include <data_access/adapters/Neo4J/Neo4JQueryVisitor.hpp>
#include <serialization/RecordJsonSerializer.hpp>

#include "data_access/adapters/Neo4J/response_processors/CloudNeo4JAuraDBResponseProcessor.hpp"
#include "data_access/adapters/Neo4J/response_processors/LocallyDeployedNeo4JResponseProcessor.hpp"

#include <data_access/adapters/cuGraph/cuGraphExample.hpp>

#include "algorithms/bfs.hpp"
#include "algorithms/RowByRowNeighborFinder.hpp"
#include <algorithm>

#include "domain/graph/Graph.hpp"

// ===== util: put terminal into raw mode so keypress works instantly =====
struct TerminalRawMode {
    termios orig{};
    TerminalRawMode() {
        tcgetattr(STDIN_FILENO, &orig);
        termios raw = orig;
        raw.c_lflag &= ~(ICANON | ECHO); // no buffering, no echo
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    ~TerminalRawMode() { tcsetattr(STDIN_FILENO, TCSANOW, &orig); }
};

std::unordered_map<std::string, DatabaseTypes> strToDBTypeMap {
    {"janus", DatabaseTypes::JANUS},
    {"arango", DatabaseTypes::ARANGO},
    {"neo", DatabaseTypes::NEO4J},
    {"cloud-neo", DatabaseTypes::CLOUD_NEO4J},
    {"mixed", DatabaseTypes::MIXED},
};

std::unordered_map<std::string, DatabaseTypes> strToDBTypeMapExternalConfig {
        {"ArangoDB", DatabaseTypes::ARANGO},
        {"Neo4J", DatabaseTypes::NEO4J},
    };


#include <domain/graph/Node.hpp>
#include <domain/graph/Edge.hpp>

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

std::unique_ptr<domain::Graph> construct_domain_graph(const std::string& filename) {

    std::ifstream ifs(filename);
    if (ifs.bad()) {
        return nullptr;
    }

    std::cout << std::filesystem::current_path() << std::endl;

    json config = json::parse(ifs);

    std::unordered_map<std::string, domain::Node*> mapper;
    auto graph = std::make_unique<domain::Graph>();

    for (auto && node : config["nodeTypes"]) {
        graph->add_node(domain::Node {
            .type = node["name"],
            .source = strToDBTypeMapExternalConfig[node["source"]],
            .properties = get_properties(node["properties"])
        });
    }

    for (domain::Node & node : graph->get_nodes()) {
        mapper[node.type] = &node;
    }

    for (auto && edge : config["relations"]) {
        auto from_it = mapper.find(edge["from"]);
        auto to_it = mapper.find(edge["to"]);

        if (from_it == mapper.end() || to_it == mapper.end()) {
            std::cout << "Not found " << edge["from"] << " or " << edge["to"] << std::endl;
            continue;
        }

        auto * from_node = from_it->second;
        auto * to_node = to_it->second;

        graph->add_edge(domain::Edge {
            .from = *from_it->second,
            .to = *to_it->second,
            .source = strToDBTypeMapExternalConfig[edge["source"]],
            .type = edge["name"],
            .properties = get_properties(edge["properties"])
        });
    }

    return graph;
}


bool comp(std::pair<uuid, size_t>& a, std::pair<uuid, size_t>& b) {
    return a.second < b.second;
};

int main(int argc, char ** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    const auto& domain_graph_filename = args[0];

    curl_global_init(CURL_GLOBAL_ALL);

    db_context_t db_context = std::make_unique<DatabaseContext>();
    db_context->planner = std::make_unique<QueryPlanner>();
    db_context->domain = construct_domain_graph(domain_graph_filename);
    auto db_registry = std::make_unique<DatabaseAdapterRegistry>();

    // ARANGO
    auto arango_ctx = std::make_unique<ExternalDatabaseContext>();
    arango_ctx->config = std::make_unique<ExternalDatabaseConfig>();
    arango_ctx->queryVisitor = std::make_unique<ArangoQueryVisitor>(*db_context);
    db_registry->register_db(DatabaseTypes::ARANGO, std::move(arango_ctx));


    // NEO4J
    auto neo4j_ctx = std::make_unique<ExternalDatabaseContext>();
    neo4j_ctx->config = std::make_unique<ExternalDatabaseConfig>();
    neo4j_ctx->queryVisitor = std::make_unique<Neo4JQueryVisitor>(
        *db_context,
        Neo4JConfig{
            .endpoint = "http://localhost:7474/db/neo4j/tx/commit",
            .response_processor = std::make_unique<LocallyDeployedNeo4JResponseProcessor>()}
        );
    db_registry->register_db(DatabaseTypes::NEO4J, std::move(neo4j_ctx));

    auto cloud_neo4j_ctx = std::make_unique<ExternalDatabaseContext>();
    cloud_neo4j_ctx->config = std::make_unique<ExternalDatabaseConfig>();
    cloud_neo4j_ctx->queryVisitor = std::make_unique<Neo4JQueryVisitor>(*
        db_context,
        Neo4JConfig{
            .endpoint = "http://localhost:9696/db/cloud-neo4j/tx/commit",
            .response_processor = std::make_unique<LocallyDeployedNeo4JResponseProcessor>()//std::make_unique<CloudNeo4JAuraDBResponseProcessor>()
            }
        );
    db_registry->register_db(DatabaseTypes::CLOUD_NEO4J, std::move(cloud_neo4j_ctx));


    db_context->db_registry = std::move(db_registry);


    auto partitioner = std::make_unique<GraphPartitioner>();

    std::pair neo_range{
        uuid{{0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
        uuid{{0x3f,0xff,0xff,0xff,0xff,0xff,0x4f,0xff,0xbf,0xff,0xff,0xff,0xff,0xff,0xff,0xff}}
    };

    std::pair arango_range {
        uuid{{0x40,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
        uuid{{0x7f,0xff,0xff,0xff,0xff,0xff,0x4f,0xff,0xbf,0xff,0xff,0xff,0xff,0xff,0xff,0xff}}
    };

    partitioner->add_range_safe(arango_range, DatabaseTypes::ARANGO);
    partitioner->add_range_safe(neo_range, DatabaseTypes::NEO4J);

    auto node_locator = std::make_unique<NodeLocator>(std::move(partitioner));

    RowByRowNeighborFinder neighbor_finder(*db_context, std::move(node_locator));
    BFS bfs { neighbor_finder};

    auto dummy_node = std::make_unique<Node>();
    dummy_node->global_id = uuid({0x04,0x7f,0xbc,0xed,0xee,0xd8,0x43,0x0b,0x8c,0x70,0xe1,0xed,0x54,0xc9,0x60,0x30});

    auto bfs_result = bfs.run(std::move(dummy_node), {});


    std::vector<std::pair<uuid, size_t>> elems(bfs_result.begin(), bfs_result.end());
    std::sort(elems.begin(), elems.end(), comp);

    std::cout << "Node,depth" << std::endl;
    for (auto&& [node_id, depth] : elems) {
        std::cout << node_id << "," << depth << std::endl;
    }

    /*
    auto comp = [](std::pair<uuid, size_t>& a, std::pair<uuid, size_t>& b) {
        return a.second < b.second;
    };
    */





    std::cout << "Controls:\n"
                 "  q = send query\n"
                 "  e = exit\n\n";

    std::cout << "Press a key...\n";
    Engine engine { std::move(db_context) };
    /*
    while (true) {
        char ch;
        std::cin >> ch;
        if (ch == 'e') break;

        if (ch == 'q') {
            std::string inputQuery;
            std::string queryTarget;
            
            std::cout << "Enter input query: "; 
            std::getline(std::cin, inputQuery);
            std::getline(std::cin, inputQuery);

            query_t query;
            if (inputQuery == "edges") {
                query = std::make_unique<GetAllEdgesQuery>();
            } else if (inputQuery == "nodes") {
                query = std::make_unique<GetAllVerticesQuery>();
            } else {
                std::cout << R"(There is no support for such query. Supported query types: ["nodes", "edges"])" << std::endl;
                continue;
            }
            

            std::cout << "Enter target DB: ";
            std::cin >> queryTarget;

            auto targetDBIt = strToDBTypeMap.find(queryTarget);
            if (targetDBIt == strToDBTypeMap.end()) {
                std::cout << "There is no support for such database!" << std::endl;
                continue;
            }

            //Query<> query {inputQuery, targetDBIt->second};
            //auto query = std::make_unique<GetAllEdgesQuery>();
            //auto query = std::make_unique<GetAllVerticesQuery>();

            auto query_start_time = std::chrono::high_resolution_clock::now();
            auto res = engine.evaluate(QueryContext {
                .query = std::move(query),
                .targetDB = targetDBIt->second
            });
            auto query_end_time = std::chrono::high_resolution_clock::now();
            json res_json = res->data;
            //auto json_convert_end_time = std::chrono::high_resolution_clock::now();

            auto query_time = std::chrono::duration_cast<std::chrono::milliseconds>(query_end_time - query_start_time).count();
            std::cout << "Query execution time: " << query_time << "ms" << std::endl;

            //auto json_convert_time = std::chrono::duration_cast<std::chrono::milliseconds>(json_convert_end_time - query_end_time).count();
            //std::cout << "JSON convertion execution time: " << json_convert_time << "ms" << std::endl;
            //execute();

            //std::cout << res_json << std::endl;
        }
    }
    */
    std::cout << "Exiting...\n";
    curl_global_cleanup();
}
