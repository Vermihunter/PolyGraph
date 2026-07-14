/**
 * @file src/infrastructure/PlacementCatalog.cpp
 * @brief Implementation for placement catalog in the backend infrastructure module.
 */

#include "infrastructure/PlacementCatalog.hpp"

#include "insertion/BackendEncodedUuid.hpp"

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

    void PlacementCatalog::set_uuid_routing_backends(std::vector<BackendId> routing_backends) {
        if (routing_backends.empty()) {
            throw std::invalid_argument("UUID routing layout must contain at least one backend");
        }
        uuid_routing_backends_ = std::move(routing_backends);
    }

    NodeRef PlacementCatalog::node_ref(const GlobalId &id) const {
        if (uuid_routing_backends_.empty()) {
            throw std::runtime_error("PlacementCatalog has no UUID routing layout");
        }

        const BackendId owner = insertion::BackendEncodedUuid::decode(id, uuid_routing_backends_);
        if (!backend_registry_->contains(owner)) {
            throw std::runtime_error("UUID routes to a backend that is not enabled");
        }

        return NodeRef{.global_id = id, .type = unresolved_type_id};
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
        (void) spec;
        (void) options;

        if (uuid_routing_backends_.empty()) {
            throw std::runtime_error("PlacementCatalog has no UUID routing layout");
        }

        const BackendId owner = insertion::BackendEncodedUuid::decode(vertex.global_id, uuid_routing_backends_);
        const auto descriptor_iterator = backend_registry_->descriptors().find(owner);
        if (descriptor_iterator == backend_registry_->descriptors().end()) {
            throw std::runtime_error("UUID routes to a backend that is not enabled");
        }

        const BackendDescriptor &descriptor = descriptor_iterator->second;
        if (!descriptor.capabilities.supports_batch_neighbors || !descriptor.coverage.fresh) {
            throw std::runtime_error("UUID-routed backend cannot execute neighbor expansion");
        }

        return {owner};
    }

} // namespace cpgdb
