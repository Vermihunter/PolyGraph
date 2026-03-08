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

int main(int argc, char ** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    curl_global_init(CURL_GLOBAL_ALL);

    db_context_t db_context = std::make_unique<DatabaseContext>();
    db_context->planner = std::make_unique<QueryPlanner>();
    auto db_registry = std::make_unique<DatabaseAdapterRegistry>();

    // ARANGO
    auto arango_ctx = std::make_unique<ExternalDatabaseContext>();
    arango_ctx->config = std::make_unique<ExternalDatabaseConfig>();
    arango_ctx->queryVisitor = std::make_unique<ArangoQueryVisitor>();
    db_registry->register_db(DatabaseTypes::ARANGO, std::move(arango_ctx));


    // NEO4J
    auto neo4j_ctx = std::make_unique<ExternalDatabaseContext>();
    neo4j_ctx->config = std::make_unique<ExternalDatabaseConfig>();
    neo4j_ctx->queryVisitor = std::make_unique<Neo4JQueryVisitor>(Neo4JConfig{
        .endpoint = "http://localhost:7474/db/neo4j/tx/commit",
        .response_processor = std::make_unique<LocallyDeployedNeo4JResponseProcessor>()
    });
    db_registry->register_db(DatabaseTypes::NEO4J, std::move(neo4j_ctx));

    auto cloud_neo4j_ctx = std::make_unique<ExternalDatabaseContext>();
    cloud_neo4j_ctx->config = std::make_unique<ExternalDatabaseConfig>();
    cloud_neo4j_ctx->queryVisitor = std::make_unique<Neo4JQueryVisitor>(Neo4JConfig{
        .endpoint = "http://localhost:9696/db/cloud-neo4j/tx/commit",
        .response_processor = std::make_unique<LocallyDeployedNeo4JResponseProcessor>()//std::make_unique<CloudNeo4JAuraDBResponseProcessor>()
    });
    db_registry->register_db(DatabaseTypes::CLOUD_NEO4J, std::move(cloud_neo4j_ctx));


    db_context->db_registry = std::move(db_registry);
    Engine engine { std::move(db_context) };

    std::cout << "Controls:\n"
                 "  q = send query\n"
                 "  e = exit\n\n";

    std::cout << "Press a key...\n";

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
                std::cout << "There is no support for such query. Supported query types: [\"nodes\", \"edges\"]" << std::endl;
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
            auto json_convert_end_time = std::chrono::high_resolution_clock::now();

            auto query_time = std::chrono::duration_cast<std::chrono::milliseconds>(query_end_time - query_start_time).count();
            std::cout << "Query execution time: " << query_time << "ms" << std::endl;

            auto json_convert_time = std::chrono::duration_cast<std::chrono::milliseconds>(json_convert_end_time - query_end_time).count();
            std::cout << "JSON convertion execution time: " << json_convert_time << "ms" << std::endl;
            //execute();

            //std::cout << res_json << std::endl;
        }
    }

    std::cout << "Exiting...\n";
    curl_global_cleanup();
}
