#ifndef QUERY_HPP
#define QUERY_HPP

#include <string>

#include <data_access/DatabaseTypes.hpp>

struct Query {
    std::string query;
    DatabaseTypes targetDB;
};

#endif // QUERY_HPP