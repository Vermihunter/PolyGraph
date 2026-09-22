/**
 * @file src/infrastructure/PlacementCatalog.cpp
 * @brief Implementation for placement catalog in the backend infrastructure module.
 */

#include "infrastructure/PlacementCatalog.hpp"

#include "insertion/BackendEncodedUuid.hpp"
#include "utilities/BackendIds.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace polygraph {

    PlacementCatalog::PlacementCatalog(std::shared_ptr<BackendRegistry> backend_registry) :
        backend_registry_(std::move(backend_registry)) {
        if (!backend_registry_) {
            throw std::invalid_argument("PlacementCatalog requires a backend registry");
        }
    }

    void PlacementCatalog::set_id_routing_backends(std::vector<BackendId> routing_backends) {
        if (routing_backends.empty()) {
            throw std::invalid_argument("ID routing layout must contain at least one backend");
        }
        id_routing_backends_ = std::move(routing_backends);
    }

    void PlacementCatalog::set_cache_residency(std::shared_ptr<const std::vector<CacheResidencyRecord>> residency) {
        if (!residency) {
            throw std::invalid_argument("External cache residency index must not be null");
        }
        if (!std::ranges::is_sorted(*residency, {}, &CacheResidencyRecord::canonical_id)) {
            throw std::invalid_argument("External cache residency must be sorted by canonical GlobalId");
        }
        cache_residency_ = std::move(residency);
        external_cache_residency_enabled_ = true;
    }

    void PlacementCatalog::set_gpu_owner_distribution(std::shared_ptr<const std::vector<GpuOwnerRecord>> ownership) {
        if (!ownership) {
            throw std::invalid_argument("GPU ownership distribution must not be null");
        }
        if (!std::ranges::is_sorted(*ownership, {}, &GpuOwnerRecord::canonical_id)) {
            throw std::invalid_argument("GPU ownership distribution must be sorted by canonical GlobalId");
        }
        gpu_owner_distribution_ = std::move(ownership);
    }

    BackendId PlacementCatalog::authoritative_owner(const GlobalId id) const {
        const BackendId logical_owner = insertion::BackendEncodedId::decode(id, id_routing_backends_);
        if (!is_gpu_backend_id(logical_owner) || !gpu_owner_distribution_) {
            return logical_owner;
        }

        const GlobalId canonical = insertion::BackendEncodedId::canonical_id(id);
        const auto &ownership = *gpu_owner_distribution_;
        const auto it = std::ranges::lower_bound(ownership, canonical, {}, &GpuOwnerRecord::canonical_id);
        if (it == ownership.end() || it->canonical_id != canonical) {
            throw std::runtime_error(
                    "Authoritative GPU vertex is missing from the physical GPU ownership distribution");
        }
        return gpu_backend_id(it->device_id);
    }

    std::uint32_t PlacementCatalog::effective_location_mask(const GlobalId id) const {
        auto locations = external_cache_residency_enabled_ ? insertion::BackendEncodedId::owner_location(id)
                                                           : insertion::BackendEncodedId::location_mask(id);

        const BackendId logical_owner = insertion::BackendEncodedId::decode(id, id_routing_backends_);
        if (is_gpu_backend_id(logical_owner) && gpu_owner_distribution_) {
            const auto logical_owner_bit = insertion::BackendEncodedId::owner_location(id);
            const auto physical_owner = authoritative_owner(id);
            locations &= ~logical_owner_bit;
            locations |= insertion::BackendEncodedId::gpu_location_bit(gpu_device_id_from_backend(physical_owner));
        }

        if (external_cache_residency_enabled_) {
            const GlobalId canonical = insertion::BackendEncodedId::canonical_id(id);
            const auto &residency = *cache_residency_;
            const auto it = std::ranges::lower_bound(residency, canonical, {}, &CacheResidencyRecord::canonical_id);
            if (it != residency.end() && it->canonical_id == canonical) {
                locations |= static_cast<insertion::BackendEncodedId::LocationMask>(it->mask);
            }
        }
        return locations;
    }

    NodeRef PlacementCatalog::node_ref(const GlobalId &id) const {
        if (id_routing_backends_.empty()) {
            throw std::runtime_error("PlacementCatalog has no ID routing layout");
        }

        const BackendId owner = authoritative_owner(id);
        if (!backend_registry_->contains(owner)) {
            throw std::runtime_error("Global ID routes to a backend that is not enabled");
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
        (void) options;

        if (id_routing_backends_.empty()) {
            throw std::runtime_error("PlacementCatalog has no ID routing layout");
        }

        // External cache residency and physical GPU ownership are independent.
        // Canonical GPU-owned IDs keep their stable logical GPU owner bit, while
        // gpu-storage/ownership.bin chooses which GPU:N actually owns the vertex.
        const auto locations = effective_location_mask(vertex.global_id);
        const BackendId owner = authoritative_owner(vertex.global_id);

        struct Candidate {
            BackendId id;
            int priority;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(backend_registry_->stores().size());

        for (const auto &[backend_id, store_value]: backend_registry_->stores()) {
            bool resident = false;
            if (is_gpu_backend_id(backend_id)) {
                resident = (locations & insertion::BackendEncodedId::gpu_location_bit(
                                                gpu_device_id_from_backend(backend_id))) != 0U;
            } else if (backend_id == REDIS) {
                resident = (locations & insertion::BackendEncodedId::REDIS_LOCATION) != 0U;
            } else if (backend_id == MMAP) {
                resident = (locations & insertion::BackendEncodedId::MMAP_LOCATION) != 0U;
            } else {
                resident = backend_id == owner;
            }
            if (!resident || !store_value->can_expand(vertex, spec)) {
                continue;
            }
            candidates.push_back(Candidate{.id = backend_id, .priority = store_value->descriptor().priority});
        }

        if (candidates.empty()) {
            throw std::runtime_error("No resident backend can execute neighbor expansion for routable vertex");
        }

        const auto best = std::ranges::min_element(candidates, [](const Candidate &left, const Candidate &right) {
            if (left.priority != right.priority) {
                return left.priority < right.priority;
            }
            return left.id < right.id;
        });
        return {best->id};
    }

} // namespace polygraph
