//
// Created by akos on 11. 7. 2026.
//

#ifndef CPGDB_NEIGHBOREXPANDER_HPP
#define CPGDB_NEIGHBOREXPANDER_HPP
#include "infrastructure/PlacementCatalog.hpp"

namespace cpgdb {
class NeighborExpander {

  public:
    explicit NeighborExpander(const PlacementCatalog& catalog)
        : catalog_(catalog) {}

    [[nodiscard]] std::vector<MinimalAdjacencyRow>
    expand_layer(const std::vector<NodeRef>& frontier,
                 const ExpandSpec& spec,
                 int depth,
                 const BfsOptions& options) const;

  private:
    [[nodiscard]] std::vector<MinimalAdjacencyRow> expand_layer_parallel(
        const PlacementCatalog::LayerExpansionPlan& plan,
        const ExpandSpec& spec,
        int depth,
        const BfsOptions& options) const;

  private:
    const PlacementCatalog& catalog_;
};
} // namespace cpgdb

#endif // CPGDB_NEIGHBOREXPANDER_HPP
