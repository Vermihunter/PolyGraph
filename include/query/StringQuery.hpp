#ifndef STRING_QUERY_HPP
#define STRING_QUERY_HPP

#include <string>

#include <data_access/DatabaseTypes.hpp>

struct StringQuery {
    std::string query;
    DatabaseTypes targetDB;
};

#endif // STRING_QUERY_HPP