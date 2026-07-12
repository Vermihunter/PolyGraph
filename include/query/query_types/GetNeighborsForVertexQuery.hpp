/**
 * @file include/query/query_types/GetNeighborsForVertexQuery.hpp
 * @brief Declarations for get neighbors for vertex query in the query dispatch module.
 */

#ifndef CPGDB_QUERY_GET_NEIGHBORS_FOR_VERTEX_QUERY_HPP
#define CPGDB_QUERY_GET_NEIGHBORS_FOR_VERTEX_QUERY_HPP

#include "data_model/Node.hpp"
#include "query/Query.hpp"

namespace cpgdb {

class GetNeighborsForVertexQuery final : public Query {
  public:
    explicit GetNeighborsForVertexQuery(Node& node) noexcept;
    [[nodiscard]] Node& node() const noexcept;
    query_result_t accept(QueryVisitor& visitor) const override;
    [[nodiscard]] query_t clone() const override;

  private:
    Node& node_;
};

} // namespace cpgdb

#endif // CPGDB_QUERY_GET_NEIGHBORS_FOR_VERTEX_QUERY_HPP
