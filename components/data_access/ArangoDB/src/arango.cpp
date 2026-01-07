#include "arango.hpp"

extern "C" std::unique_ptr<IDatabase> create_database() {
    return std::make_unique<ArangoDB>();
}