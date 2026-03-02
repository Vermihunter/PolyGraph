#include <iostream>
#include <dlfcn.h>
#include <unistd.h>
#include <termios.h>
#include <vector>
#include <curl/curl.h>
#include <unordered_map>

#include <engine/Engine.hpp>
#include <data_access/IDatabase.hpp>
#include <data_access/adapters/JanusGraph/janus.hpp>
#include <data_access/adapters/ArangoDB/arango.hpp>

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
    {"mixed", DatabaseTypes::MIXED},
};

int main(int argc, char ** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    // if (args.size() != 2) {
    //     std::cout << "Usage:" << std::endl;
    //     std::cout << "  ./src/core/graph_example [[JANUSGRAPH_DB_IMPLEMENTATION.so]] [[ARANGODB_DB_IMPLEMENTATION.so]]";
    //     exit(1);
    // }

    curl_global_init(CURL_GLOBAL_ALL);


    const std::string& janusgraph_so = args[0];
    const std::string& arangodb_so = args[1];

    //TerminalRawMode raw; // restore on exit


    db_context_t db_context = std::make_unique<DatabaseContext>();
    db_context->planner = std::make_unique<QueryPlanner>();
    auto db_registry = std::make_unique<DatabaseAdapterRegistry>();
    db_registry->register_db(DatabaseTypes::ARANGO, std::make_unique<ArangoDB>());
    db_registry->register_db(DatabaseTypes::JANUS, std::make_unique<JanusGraph>());

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

            std::cout << "Enter input query: " << std::endl;
            std::getline(std::cin, inputQuery);
            std::getline(std::cin, inputQuery);
            

            std::cout << "Enter target DB:" << std::endl;
            std::cin >> queryTarget;

            auto targetDBIt = strToDBTypeMap.find(queryTarget);
            if (targetDBIt == strToDBTypeMap.end()) {
                std::cout << "There is no support for such database!" << std::endl;
                continue;
            }

            Query query {inputQuery, targetDBIt->second};
            QueryResult res = engine.evaluate(query);

            std::cout << res.hardCodedResultData << std::endl;
        }
    }

    std::cout << "Exiting...\n";
    curl_global_cleanup();
}
