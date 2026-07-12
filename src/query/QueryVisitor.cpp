/**
 * @file src/query/QueryVisitor.cpp
 * @brief Implementation for query visitor in the query dispatch module.
 */

#include "query/QueryVisitor.hpp"

#include <stdexcept>

namespace cpgdb {

    QueryVisitor::QueryVisitor(DatabaseContext &database_context) noexcept : db_context_(database_context) {}

    QueryVisitor::~QueryVisitor() = default;

    query_result_t QueryVisitor::visit(const InsertQuery &) {
        throw std::logic_error("This query visitor does not support InsertQuery");
    }

    query_result_t QueryVisitor::visit(const InsertBatchQuery &) {
        throw std::logic_error("This query visitor does not support InsertBatchQuery");
    }

} // namespace cpgdb
