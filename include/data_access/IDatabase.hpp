#ifndef IDATABASE_HPP
#define IDATABASE_HPP

#include <string>
#include <memory>

class IDatabase {

public:
    virtual std::string evaluate(const std::string& query) = 0;
};


extern "C" {
    typedef std::unique_ptr<IDatabase> (*create_database_t)();
}

#endif // IDATABASE_HPP