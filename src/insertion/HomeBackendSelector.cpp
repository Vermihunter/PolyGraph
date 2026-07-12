/**
 * @file src/insertion/HomeBackendSelector.cpp
 * @brief Implementation for home backend selector in the insertion planning module.
 */

#include "insertion/HomeBackendSelector.hpp"

#include <limits>
#include <stdexcept>

namespace cpgdb::insertion {

    IHomeBackendSelector::~IHomeBackendSelector() = default;

    LeastLoadedHomeBackendSelector::LeastLoadedHomeBackendSelector() : LeastLoadedHomeBackendSelector(Weights{}) {}

    LeastLoadedHomeBackendSelector::LeastLoadedHomeBackendSelector(Weights weights) : weights_(weights) {}

    BackendId LeastLoadedHomeBackendSelector::choose_for_node(const Node &, const std::vector<BackendId> &candidates,
                                                              PlacementContext &context) const {
        return choose(candidates, context, EntityKind::Node);
    }

    BackendId LeastLoadedHomeBackendSelector::choose_for_edge(const Edge &, const std::vector<BackendId> &candidates,
                                                              PlacementContext &context) const {
        return choose(candidates, context, EntityKind::Edge);
    }

    BackendId LeastLoadedHomeBackendSelector::choose(const std::vector<BackendId> &candidates,
                                                     PlacementContext &context, const EntityKind kind) const {
        if (candidates.empty()) {
            throw std::invalid_argument("Home backend selection requires at least one candidate");
        }

        BackendId best_backend = candidates.front();
        double best_score = std::numeric_limits<double>::infinity();

        for (const BackendId backend_id: candidates) {
            if (!context.backend_registry.contains(backend_id)) {
                throw std::runtime_error("Home backend selector received an unknown backend id");
            }
            const BackendLoad load = context.load_snapshot.load(backend_id);
            const int priority = context.backend_registry.descriptor(backend_id).priority;
            const double candidate_score = score(load, priority, kind);
            if (candidate_score < best_score || (candidate_score == best_score && backend_id < best_backend)) {
                best_backend = backend_id;
                best_score = candidate_score;
            }
        }

        return best_backend;
    }

    double LeastLoadedHomeBackendSelector::score(const BackendLoad &load, const int backend_priority,
                                                 const EntityKind kind) const noexcept {
        const double ownership_score = kind == EntityKind::Node
                                               ? static_cast<double>(load.owned_nodes) * weights_.owned_nodes
                                               : static_cast<double>(load.owned_edges) * weights_.owned_edges;
        const double storage_score = static_cast<double>(load.stored_nodes) * weights_.stored_nodes +
                                     static_cast<double>(load.stored_edges) * weights_.stored_edges;
        return ownership_score + storage_score + static_cast<double>(backend_priority) * weights_.backend_priority;
    }

} // namespace cpgdb::insertion
