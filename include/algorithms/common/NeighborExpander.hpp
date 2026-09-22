//
// Created by akos on 11. 7. 2026.
//

#ifndef POLYGRAPH_NEIGHBOREXPANDER_HPP
#define POLYGRAPH_NEIGHBOREXPANDER_HPP
#include "infrastructure/PlacementCatalog.hpp"
#include "data_access/NeighborBatchConsumer.hpp"

namespace polygraph {
class NeighborExpander {

  public:
    explicit NeighborExpander(const PlacementCatalog& catalog)
        : catalog_(catalog) {}

    [[nodiscard]] std::vector<MinimalAdjacencyRow>
    expand_layer(const std::vector<NodeRef>& frontier,
                 const ExpandSpec& spec,
                 int depth,
                 const BfsOptions& options) const;

    /** @brief Streams candidate neighbor IDs without materializing complete adjacency rows. */
    void expand_layer_candidates(const std::vector<NodeRef>& frontier,
                                 const ExpandSpec& spec,
                                 int depth,
                                 const BfsOptions& options,
                                 const NeighborBatchConsumer& consumer) const;

  private:
    [[nodiscard]] std::vector<MinimalAdjacencyRow> expand_layer_parallel(
        const PlacementCatalog::LayerExpansionPlan& plan,
        const ExpandSpec& spec,
        int depth,
        const BfsOptions& options) const;

  private:
    const PlacementCatalog& catalog_;
};
} // namespace polygraph

#endif // POLYGRAPH_NEIGHBOREXPANDER_HPP
