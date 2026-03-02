#ifndef QUERY_RESULT_HPP
#define QUERY_RESULT_HPP

#include <vector>

#include <query/QueryResultMatch.hpp>

struct QueryResult {
    std::vector<QueryResultMatch> data;


    // rework after
    std::string hardCodedResultData;
};

#endif // QUERY_RESULT_HPP