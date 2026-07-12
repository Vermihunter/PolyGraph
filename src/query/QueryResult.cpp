/**
 * @file src/query/QueryResult.cpp
 * @brief Implementation for query result in the query dispatch module.
 */

#include "query/QueryResult.hpp"

#include <iterator>
#include <stdexcept>
#include <utility>

namespace cpgdb {

    void QueryResult::combine(query_result_t other) {
        if (!other) {
            throw std::invalid_argument("Cannot combine a null QueryResult");
        }
        data.reserve(data.size() + other->data.size());
        std::move(other->data.begin(), other->data.end(), std::back_inserter(data));
    }

} // namespace cpgdb
