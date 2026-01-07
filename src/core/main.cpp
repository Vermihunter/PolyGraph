#include <iostream>
#include <dlfcn.h>
#include <unistd.h>
#include <termios.h>
#include <vector>
#include <curl/curl.h>
#include "../../include/data_access/IDatabase.hpp"


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

// ===== helper to load a plugin and create a Shape =====
struct Plugin {
    void* handle = nullptr;
    create_database_t create = nullptr;
    std::unique_ptr<IDatabase> instance = nullptr;

    bool load(const std::string& path) {
        unload(); // ensure clean state

        handle = dlopen(path.c_str(), RTLD_LAZY);
        if (!handle) {
            std::cerr << "dlopen failed: " << dlerror() << "\n";
            return false;
        }

        create = (create_database_t)dlsym(handle, "create_database");

        if (!create) {
            std::cerr << "Failed loading factory functions\n";
            dlclose(handle);
            handle = nullptr;
            return false;
        }

        instance = create();
        return true;
    }

    void unload() {
        instance = nullptr;

        if (handle) dlclose(handle);
        handle = nullptr;
        create = nullptr;
    }

    ~Plugin() { unload(); }
};

int main(int argc, char ** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() != 2) {
        std::cout << "Usage:" << std::endl;
        std::cout << "  ./src/core/graph_example [[JANUSGRAPH_DB_IMPLEMENTATION.so]] [[ARANGODB_DB_IMPLEMENTATION.so]]";
        exit(1);
    }

    curl_global_init(CURL_GLOBAL_ALL);


    const std::string& janusgraph_so = args[0];
    const std::string& arangodb_so = args[1];

    TerminalRawMode raw; // restore on exit

    Plugin plugin;

    std::cout << "Controls:\n"
                 "  j = use JanusGraph DB as storage\n"
                 "  a = use ArangoDB DB as storage\n"
                 "  e = evaluate query with currently loaded DB engine"
                 "  q = quit\n\n";

    std::cout << "Press a key...\n";

    while (true) {
        char ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) continue;

        if (ch == 'q') break;

        const std::string* so;

        if (ch == 'j') {
            so = &janusgraph_so;
            std::cout << "Loading JanusGraph...\n";
        } else if (ch == 'a') {
            so = &arangodb_so;
            std::cout << "Loading ArangoDB...\n";
        } else if (ch == 'e') {
            std::cout << plugin.instance->evaluate("Query") << std::endl;
            continue;
        } else {
            continue;
        }

        if (!plugin.load(*so)) {
            std::cout << "Failed to load plugin " << *so << std::endl;
        } else {
            std::cout << "Successfully loaded plugin " << *so << std::endl;
        }
    }

    std::cout << "Exiting...\n";
    curl_global_cleanup();
}
