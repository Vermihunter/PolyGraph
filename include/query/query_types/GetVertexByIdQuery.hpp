/**
 * @file include/query/query_types/GetVertexByIdQuery.hpp
 * @brief Declarations for get vertex by id query in the query dispatch module.
 */

#ifndef POLYGRAPH_QUERY_GET_VERTEX_BY_ID_QUERY_HPP
#define POLYGRAPH_QUERY_GET_VERTEX_BY_ID_QUERY_HPP

#include "data_model/Node.hpp"
#include "query/Query.hpp"

namespace polygraph {

class GetVertexByIdQuery final : public Query {
  public:
    explicit GetVertexByIdQuery(Node& node) noexcept;
    [[nodiscard]] Node& node() const noexcept;
    query_result_t accept(QueryVisitor& visitor) const override;
    [[nodiscard]] query_t clone() const override;

  private:
    Node& node_;
};

} // namespace polygraph

#endif // POLYGRAPH_QUERY_GET_VERTEX_BY_ID_QUERY_HPP
