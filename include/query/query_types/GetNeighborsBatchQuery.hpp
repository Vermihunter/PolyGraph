/**
 * @file include/query/query_types/GetNeighborsBatchQuery.hpp
 * @brief Declarations for get neighbors batch query in the query dispatch module.
 */

#ifndef POLYGRAPH_QUERY_GET_NEIGHBORS_BATCH_QUERY_HPP
#define POLYGRAPH_QUERY_GET_NEIGHBORS_BATCH_QUERY_HPP

#include "algorithms/ExpandSpec.hpp"
#include "data_model/NodeRef.hpp"
#include "query/Query.hpp"

#include <vector>

namespace polygraph {

class GetNeighborsBatchQuery final : public Query {
  public:
    GetNeighborsBatchQuery(std::vector<NodeRef> frontier, ExpandSpec spec);

    [[nodiscard]] const std::vector<NodeRef>& frontier() const noexcept;
    [[nodiscard]] const ExpandSpec& spec() const noexcept;
    query_result_t accept(QueryVisitor& visitor) const override;
    [[nodiscard]] query_t clone() const override;

  private:
    std::vector<NodeRef> frontier_;
    ExpandSpec spec_;
};

} // namespace polygraph

#endif // POLYGRAPH_QUERY_GET_NEIGHBORS_BATCH_QUERY_HPP
