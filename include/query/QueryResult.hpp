#ifndef QUERY_RESULT_HPP
#define QUERY_RESULT_HPP

#include <vector>
#include <memory>

#include <data_model/Record.hpp>

struct QueryResult;
using query_result_t = std::unique_ptr<QueryResult>;

struct QueryResult {
    std::vector<record_t> data;

    void combine(query_result_t&& other) {
        for (auto&& r : other->data) {
            data.push_back(std::move(r));
        }
    }
};


#endif // QUERY_RESULT_HPP