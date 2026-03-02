#ifndef IDATABASE_HPP
#define IDATABASE_HPP

#include <string>
#include <memory>

#include <query/QueryResult.hpp>
#include <query/Query.hpp>

class IDatabase {

public:
    virtual QueryResult evaluate(const Query& query) = 0;
};

typedef std::unique_ptr<IDatabase> database_t;


// extern "C" {
//     typedef database_t (*create_database_t)();
// }

#endif // IDATABASE_HPP