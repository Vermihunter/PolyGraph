/**
 * @file include/query/query_types/GetAllEdgesQuery.hpp
 * @brief Declarations for get all edges query in the query dispatch module.
 */

#ifndef CPGDB_QUERY_GET_ALL_EDGES_QUERY_HPP
#define CPGDB_QUERY_GET_ALL_EDGES_QUERY_HPP

#include "query/Query.hpp"

namespace cpgdb {

class GetAllEdgesQuery final : public Query {
  public:
    query_result_t accept(QueryVisitor& visitor) const override;
    [[nodiscard]] query_t clone() const override;
};

} // namespace cpgdb

#endif // CPGDB_QUERY_GET_ALL_EDGES_QUERY_HPP
