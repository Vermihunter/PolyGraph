//
// Created by akos on 30. 4. 2026.
//

#ifndef CPGDB_PLACEMENTCATALOG_HPP
#define CPGDB_PLACEMENTCATALOG_HPP

#include <optional>
#include <utility>
#include <vector>

#include "BackendDescriptor.hpp"
#include "BackendRegistry.hpp"
#include "Placement.hpp"
#include "algorithms/bfs/BfsOptions.hpp"
#include "data_model/EntityId.hpp"
#include "domain/graph/TypeId.hpp"
#include "utilities/BackendIds.hpp"

class PlacementCatalog {
public:
    explicit PlacementCatalog(std::shared_ptr<BackendRegistry> backend_registry)
        : backend_registry_(std::move(backend_registry)) {}

    void register_placement(Placement placement) {
        placements_.push_back(std::move(placement));
    }

    [[nodiscard]] std::shared_ptr<IGraphStore>
    store(BackendId id) const {
        auto it = backend_registry_->get_stores().find(id);

        if (it == backend_registry_->get_stores().end()) {
            throw std::runtime_error("Unknown backend id");
        }

        return it->second;
    }

    [[nodiscard]] std::optional<BackendId>
    choose_native_bfs_backend(const NodeRef& source,
                              const ExpandSpec& spec,
                              const BfsOptions& options) const {
        std::vector<BackendDescriptor> candidates;

        for (const auto& [backend_id, store] : backend_registry_->get_stores()) {
            if (store->can_run_native_bfs(source, spec, options)) {
                candidates.push_back(store->descriptor());
            }
        }

        if (candidates.empty()) {
            return std::nullopt;
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const BackendDescriptor& a, const BackendDescriptor& b) {
                return a.priority < b.priority;
            }
        );

        return candidates.front().id;
    }

    struct LayerExpansionPlan {
        std::unordered_map<BackendId, std::vector<NodeRef>> backend_frontiers;
    };

    /*
    [[nodiscard]] LayerExpansionPlan
    plan_frontier_expansion(std::span<const NodeRef> frontier,
                            const ExpandSpec& spec,
                            const BfsOptions& options) const {
        LayerExpansionPlan plan;

        for (const auto& vertex : frontier) {
            auto backend_ids = choose_expansion_backends_for_vertex(
                vertex,
                spec,
                options
            );

            //if (vertex.type == backend_registry_->)

            for (BackendId backend_id : backend_ids) {
                plan.backend_frontiers[backend_id].push_back(vertex);
            }
        }

        return plan;
    }
    */

    [[nodiscard]] LayerExpansionPlan
    plan_frontier_expansion(std::span<const NodeRef> frontier,
                        const ExpandSpec& spec,
                        const BfsOptions& options) const {
        LayerExpansionPlan plan;

        for (const auto& vertex : frontier) {
            auto backend_ids = choose_expansion_backends_for_vertex(
                vertex,
                spec,
                options
            );

            std::unordered_set<BackendId> seen_backends;

            for (BackendId backend_id : backend_ids) {
                if (seen_backends.insert(backend_id).second) {
                    plan.backend_frontiers[backend_id].push_back(vertex);
                }
            }
        }

        return plan;
    }

private:
    [[nodiscard]] std::vector<BackendId>
    choose_expansion_backends_for_vertex(const NodeRef& vertex,
                                         const ExpandSpec& spec,
                                         const BfsOptions& options) const {

        /*
        if (vertex.type == 18) {
            std::cout << "Halo itt vok" << std::endl;
        }

        if (backend_registry_->descriptor(ARANGO).coverage.node_types.contains(vertex.type)) {
            throw std::runtime_error("");
        }
        */

        std::vector<Placement> matching;

        for (const auto& placement : placements_) {
            if (!placement.can_expand_from(vertex, spec)) {
                continue;
            }

            auto descriptor_it = backend_registry_->descriptors().find(placement.backend_id);
            if (descriptor_it == backend_registry_->descriptors().end()) {
                continue;
            }

            const auto& descriptor = descriptor_it->second;

            if (!descriptor.capabilities.supports_batch_neighbors) {
                continue;
            }

            if (!descriptor.coverage.fresh) {
                continue;
            }

            matching.push_back(placement);
        }

        if (matching.empty()) {
            return {};
        }

        std::sort(
            matching.begin(),
            matching.end(),
            [](const Placement& a, const Placement& b) {
                return a.priority < b.priority;
            }
        );

        /*
         * Cache/projection shortcut:
         *
         * If Memgraph or GPU projection is complete for this requested expansion,
         * use it and avoid querying the persistent primary as duplicate work.
         */
        if (options.allow_partial_acceleration) {
            for (const auto& placement : matching) {
                const auto& descriptor = backend_registry_->descriptor(placement.backend_id);

                bool accelerated =
                    placement.kind == PlacementKind::Cache ||
                    placement.kind == PlacementKind::GpuProjection;

                if (accelerated &&
                    descriptor.coverage.covers_expand_spec(vertex, spec)) {
                    return {placement.backend_id};
                }
            }
        }

        /*
         * Otherwise use authoritative placements.
         *
         * Important:
         * A vertex may need to be sent to multiple authoritative stores if its
         * edge types are partitioned across backends.
         */
        std::vector<BackendId> authoritative;

        for (const auto& placement : matching) {
            if (placement.authoritative) {
                authoritative.push_back(placement.backend_id);
            }
        }

        if (!authoritative.empty()) {
            return authoritative;
        }

        /*
         * Last-resort fallback: use the best matching placement.
         */
        return {matching.front().backend_id};
    }

    std::shared_ptr<BackendRegistry> backend_registry_;
    std::vector<Placement> placements_;
};
#endif //CPGDB_PLACEMENTCATALOG_HPP