/**
 * @file include/insertion/InsertionCoordinator.hpp
 * @brief Application-facing orchestration of policy-driven multi-backend writes.
 */

#ifndef POLYGRAPH_INSERTION_INSERTION_COORDINATOR_HPP
#define POLYGRAPH_INSERTION_INSERTION_COORDINATOR_HPP

#include "data_model/Edge.hpp"
#include "data_model/Node.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "insertion/BackendLoadTracker.hpp"
#include "insertion/InsertionPlacementPolicy.hpp"

#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace polygraph::insertion {

    /// Move-owning input variant accepted by mixed insertion batches.
    using OwnedInsertionEntity = std::variant<Node, Edge>;

    /** @brief Per-entity outcome returned after all selected backend writes succeed. */
    struct InsertionReceipt {
        GlobalId global_id; ///< Final ID after generation/ownership encoding.
        EntityKind kind; ///< Node or edge category.
        TypeId type; ///< Logical entity type.
        std::vector<BackendId> target_backends; ///< Stores that received the entity.
        std::optional<BackendId> home_backend; ///< Selected logical owner, when any.
    };

    /**
     * @brief Plans, validates, batches, executes, and accounts for graph insertions.
     *
     * The coordinator is deliberately backend-neutral. It emits one
     * InsertBatchQuery per target backend through IGraphStore::execute(). Load
     * counters are committed only after every backend call succeeds.
     *
     * This class does not provide a distributed transaction. Callers requiring
     * atomic cross-database writes should add idempotency and an outbox, saga, or
     * compensation mechanism.
     */
    class InsertionCoordinator {
    public:
        /**
         * @brief Creates a coordinator from a live runtime, placement policy, and tracker.
         * @throws std::invalid_argument when either shared strategy dependency is null.
         */
        InsertionCoordinator(DatabaseContext &database_context,
                             std::shared_ptr<const IInsertionPlacementPolicy> placement_policy,
                             std::shared_ptr<IBackendLoadTracker> load_tracker);

        /** @brief Inserts one node according to the active placement policy. */
        [[nodiscard]] InsertionReceipt insert(Node node);
        /**
         * @brief Inserts one node with an explicit backend-local integer ID.
         *
         * Passing 0, 1, ..., N-1 lets callers create a compact contiguous Neo4j
         * ``localId`` range while the coordinator adds the home-backend routing bits.
         */
        [[nodiscard]] InsertionReceipt insert(Node node, GlobalId backend_local_id);
        /** @brief Inserts one edge according to the active placement policy. */
        [[nodiscard]] InsertionReceipt insert(Edge edge);
        /** @brief Inserts one edge with an explicit backend-local integer ID. */
        [[nodiscard]] InsertionReceipt insert(Edge edge, GlobalId backend_local_id);
        /** @brief Inserts a mixed batch while grouping physical writes by backend. */
        [[nodiscard]] std::vector<InsertionReceipt> insert_batch(std::vector<OwnedInsertionEntity> entities);

    private:
        /** @brief Internal immutable payload paired with its validated placement decision. */
        struct PlannedEntity {
            InsertionItem item;
            PlacementDecision decision;
        };

        [[nodiscard]] PlannedEntity plan(Node node, PlacementContext &context,
                                         std::optional<GlobalId> backend_local_id = std::nullopt) const;
        [[nodiscard]] PlannedEntity plan(Edge edge, PlacementContext &context,
                                         std::optional<GlobalId> backend_local_id = std::nullopt) const;
        [[nodiscard]] static GlobalId prepare_id(GlobalId id, const PlacementDecision &decision,
                                                 std::span<const BackendId> routing_backends);
        [[nodiscard]] static GlobalId prepare_local_id(GlobalId backend_local_id, const PlacementDecision &decision,
                                                       std::span<const BackendId> routing_backends);
        void execute(const std::vector<PlannedEntity> &planned) const;
        void record_committed(const std::vector<PlannedEntity> &planned) const;
        [[nodiscard]] static InsertionReceipt receipt_for(const PlannedEntity &planned);

        DatabaseContext &database_context_;
        std::shared_ptr<const IInsertionPlacementPolicy> placement_policy_;
        std::shared_ptr<IBackendLoadTracker> load_tracker_;
    };

} // namespace polygraph::insertion

#endif // POLYGRAPH_INSERTION_INSERTION_COORDINATOR_HPP
