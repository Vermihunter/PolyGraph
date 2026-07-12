/**
 * @file src/infrastructure/PlacementCatalog.cpp
 * @brief Implementation for placement catalog in the backend infrastructure module.
 */

#include "infrastructure/PlacementCatalog.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace cpgdb {

    PlacementCatalog::PlacementCatalog(std::shared_ptr<BackendRegistry> backend_registry) :
        backend_registry_(std::move(backend_registry)) {
        if (!backend_registry_) {
            throw std::invalid_argument("PlacementCatalog requires a backend registry");
        }
    }

    void PlacementCatalog::register_placement(Placement placement) { placements_.push_back(std::move(placement)); }

    std::shared_ptr<IGraphStore> PlacementCatalog::store(BackendId id) const {
        return backend_registry_->get_store(id);
    }

    std::optional<BackendId> PlacementCatalog::choose_native_bfs_backend(const NodeRef &source, const ExpandSpec &spec,
                                                                         const BfsOptions &options) const {
        std::vector<BackendDescriptor> candidates;
        candidates.reserve(backend_registry_->stores().size());

        for (const auto &[backend_id, store_value]: backend_registry_->stores()) {
            (void) backend_id;
            if (store_value->can_run_native_bfs(source, spec, options)) {
                candidates.push_back(store_value->descriptor());
            }
        }

        if (candidates.empty()) {
            return std::nullopt;
        }

        const auto best = std::ranges::min_element(candidates, {}, &BackendDescriptor::priority);
        return best->id;
    }

    PlacementCatalog::LayerExpansionPlan PlacementCatalog::plan_frontier_expansion(std::span<const NodeRef> frontier,
                                                                                   const ExpandSpec &spec,
                                                                                   const BfsOptions &options) const {
        LayerExpansionPlan plan;

        for (const NodeRef &vertex: frontier) {
            const auto backend_ids = choose_expansion_backends_for_vertex(vertex, spec, options);
            std::unordered_set<BackendId> seen_backends;
            seen_backends.reserve(backend_ids.size());

            for (const BackendId backend_id: backend_ids) {
                if (seen_backends.insert(backend_id).second) {
                    plan.backend_frontiers[backend_id].push_back(vertex);
                }
            }
        }

        return plan;
    }

    std::vector<BackendId> PlacementCatalog::choose_expansion_backends_for_vertex(const NodeRef &vertex,
                                                                                  const ExpandSpec &spec,
                                                                                  const BfsOptions &options) const {
        std::vector<const Placement *> matching;
        matching.reserve(placements_.size());

        for (const Placement &placement: placements_) {
            if (!placement.can_expand_from(vertex, spec)) {
                continue;
            }

            const auto descriptor_iterator = backend_registry_->descriptors().find(placement.backend_id);
            if (descriptor_iterator == backend_registry_->descriptors().end()) {
                continue;
            }

            const BackendDescriptor &descriptor_value = descriptor_iterator->second;
            if (!descriptor_value.capabilities.supports_batch_neighbors || !descriptor_value.coverage.fresh) {
                continue;
            }

            matching.push_back(&placement);
        }

        if (matching.empty()) {
            return {};
        }

        std::ranges::sort(matching, {}, [](const Placement *placement) { return placement->priority; });

        if (options.allow_partial_acceleration) {
            for (const Placement *placement: matching) {
                const BackendDescriptor &descriptor_value = backend_registry_->descriptor(placement->backend_id);
                const bool accelerated =
                        placement->kind == PlacementKind::Cache || placement->kind == PlacementKind::GpuProjection;

                if (accelerated && descriptor_value.coverage.covers_expand_spec(vertex, spec)) {
                    return {placement->backend_id};
                }
            }
        }

        std::vector<BackendId> authoritative;
        authoritative.reserve(matching.size());
        for (const Placement *placement: matching) {
            if (placement->authoritative) {
                authoritative.push_back(placement->backend_id);
            }
        }

        if (!authoritative.empty()) {
            return authoritative;
        }

        return {matching.front()->backend_id};
    }

} // namespace cpgdb
