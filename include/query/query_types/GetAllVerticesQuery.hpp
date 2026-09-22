/**
 * @file include/query/query_types/GetAllVerticesQuery.hpp
 * @brief Declarations for get all vertices query in the query dispatch module.
 */

#ifndef POLYGRAPH_QUERY_GET_ALL_VERTICES_QUERY_HPP
#define POLYGRAPH_QUERY_GET_ALL_VERTICES_QUERY_HPP

#include "query/Query.hpp"

namespace polygraph {

class GetAllVerticesQuery final : public Query {
  public:
    query_result_t accept(QueryVisitor& visitor) const override;
    [[nodiscard]] query_t clone() const override;
};

} // namespace polygraph

#endif // POLYGRAPH_QUERY_GET_ALL_VERTICES_QUERY_HPP
