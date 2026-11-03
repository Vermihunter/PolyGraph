/**
 * @file include/insertion/HomeBackendSelector.hpp
 * @brief Strategies that select logical ownership independently from replica targets.
 */

#ifndef POLYGRAPH_INSERTION_HOME_BACKEND_SELECTOR_HPP
#define POLYGRAPH_INSERTION_HOME_BACKEND_SELECTOR_HPP

#include "data_model/Edge.hpp"
#include "data_model/Node.hpp"
#include "insertion/InsertionPlacementPolicy.hpp"

#include <vector>

namespace polygraph::insertion {

/** @brief Interface for selecting one home backend from a fixed candidate set. */
class IHomeBackendSelector {
  public:
    virtual ~IHomeBackendSelector();

    /** @brief Chooses and reserves logical ownership for @p node. */
    [[nodiscard]] virtual BackendId choose_for_node(const Node& node,
                                                    const std::vector<BackendId>& candidates,
                                                    PlacementContext& context) const = 0;
    /** @brief Chooses and reserves logical ownership for @p edge. */
    [[nodiscard]] virtual BackendId choose_for_edge(const Edge& edge,
                                                    const std::vector<BackendId>& candidates,
                                                    PlacementContext& context) const = 0;
};

/** @brief Weighted least-loaded ownership selector with deterministic priority tie-breaking. */
class LeastLoadedHomeBackendSelector final : public IHomeBackendSelector {
  public:
    /** @brief Coefficients used to calculate one backend's placement score. */
    struct Weights {
        double owned_nodes = 1.0;
        double owned_edges = 1.0;
        double stored_nodes = 0.0;
        double stored_edges = 0.0;
        double backend_priority = 0.001;
    };

    /** @brief Creates a selector with ownership-focused default weights. */
    LeastLoadedHomeBackendSelector();
    /** @brief Creates a selector with caller-defined @p weights. */
    explicit LeastLoadedHomeBackendSelector(Weights weights);

    [[nodiscard]] BackendId choose_for_node(const Node& node,
                                            const std::vector<BackendId>& candidates,
                                            PlacementContext& context) const override;
    [[nodiscard]] BackendId choose_for_edge(const Edge& edge,
                                            const std::vector<BackendId>& candidates,
                                            PlacementContext& context) const override;

  private:
    [[nodiscard]] BackendId choose(const std::vector<BackendId>& candidates,
                                   PlacementContext& context,
                                   EntityKind kind) const;
    [[nodiscard]] double
    score(const BackendLoad& load, int backend_priority, EntityKind kind) const noexcept;

    Weights weights_;
};

} // namespace polygraph::insertion

#endif // POLYGRAPH_INSERTION_HOME_BACKEND_SELECTOR_HPP
