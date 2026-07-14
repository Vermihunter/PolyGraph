/**
 * @file src/insertion/InsertionCoordinator.cpp
 * @brief Implementation for insertion coordinator in the insertion planning module.
 */

#include "insertion/InsertionCoordinator.hpp"

#include "insertion/BackendEncodedUuid.hpp"
#include "query/query_types/InsertBatchQuery.hpp"

#include <algorithm>
#include <boost/uuid/random_generator.hpp>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace cpgdb::insertion {

    InsertionCoordinator::InsertionCoordinator(DatabaseContext &database_context,
                                               std::shared_ptr<const IInsertionPlacementPolicy> placement_policy,
                                               std::shared_ptr<IBackendLoadTracker> load_tracker) :
        database_context_(database_context), placement_policy_(std::move(placement_policy)),
        load_tracker_(std::move(load_tracker)) {
        if (!database_context_.backend_registry) {
            throw std::invalid_argument("InsertionCoordinator requires a backend registry");
        }
        if (!placement_policy_) {
            throw std::invalid_argument("InsertionCoordinator requires a placement policy");
        }
        if (!load_tracker_) {
            throw std::invalid_argument("InsertionCoordinator requires a load tracker");
        }
    }

    InsertionReceipt InsertionCoordinator::insert(Node node) {
        std::vector<OwnedInsertionEntity> entities;
        entities.emplace_back(std::move(node));
        auto receipts = insert_batch(std::move(entities));
        return std::move(receipts.front());
    }

    InsertionReceipt InsertionCoordinator::insert(Edge edge) {
        std::vector<OwnedInsertionEntity> entities;
        entities.emplace_back(std::move(edge));
        auto receipts = insert_batch(std::move(entities));
        return std::move(receipts.front());
    }

    std::vector<InsertionReceipt> InsertionCoordinator::insert_batch(std::vector<OwnedInsertionEntity> entities) {
        if (entities.empty()) {
            return {};
        }

        BackendLoadSnapshot load_snapshot(*load_tracker_);
        PlacementContext context{.backend_registry = *database_context_.backend_registry,
                                 .load_snapshot = load_snapshot};

        std::vector<PlannedEntity> planned;
        planned.reserve(entities.size());
        for (auto &entity: entities) {
            PlannedEntity planned_entity =
                    std::visit([this, &context](auto &value) { return plan(std::move(value), context); }, entity);
            if (planned_entity.decision.home_backend.has_value()) {
                context.load_snapshot.reserve_owned(*planned_entity.decision.home_backend, planned_entity.item.kind());
            }
            planned.push_back(std::move(planned_entity));
        }

        execute(planned);
        record_committed(planned);

        std::vector<InsertionReceipt> receipts;
        receipts.reserve(planned.size());
        for (const PlannedEntity &entity: planned) {
            receipts.push_back(receipt_for(entity));
        }
        return receipts;
    }

    InsertionCoordinator::PlannedEntity InsertionCoordinator::plan(Node node, PlacementContext &context) const {
        PlacementDecision decision = placement_policy_->place_node(node, context);
        validate_placement_decision(decision, context.backend_registry);
        node.global_id = prepare_id(node.global_id, decision, database_context_.uuid_routing_backends);
        return PlannedEntity{.item = InsertionItem(std::make_shared<const Node>(std::move(node))),
                             .decision = std::move(decision)};
    }

    InsertionCoordinator::PlannedEntity InsertionCoordinator::plan(Edge edge, PlacementContext &context) const {
        PlacementDecision decision = placement_policy_->place_edge(edge, context);
        validate_placement_decision(decision, context.backend_registry);
        edge.global_id = prepare_id(edge.global_id, decision, database_context_.uuid_routing_backends);
        return PlannedEntity{.item = InsertionItem(std::make_shared<const Edge>(std::move(edge))),
                             .decision = std::move(decision)};
    }

    GlobalId InsertionCoordinator::prepare_id(GlobalId id, const PlacementDecision &decision,
                                              const std::span<const BackendId> routing_backends) {
        if (id.is_nil()) {
            id = boost::uuids::random_generator{}();
        }
        if (decision.encode_home_backend_in_uuid) {
            return BackendEncodedUuid::encode(id, *decision.home_backend, routing_backends);
        }
        return id;
    }

    void InsertionCoordinator::execute(const std::vector<PlannedEntity> &planned) const {
        std::map<BackendId, std::vector<InsertionItem>> batches;
        for (const PlannedEntity &entity: planned) {
            for (const BackendId backend_id: entity.decision.target_backends) {
                batches[backend_id].push_back(entity.item);
            }
        }

        for (auto &[backend_id, items]: batches) {
            try {
                auto result = database_context_.backend_registry->get_store(backend_id)
                                      ->execute(InsertBatchQuery(std::move(items)));
                if (!result) {
                    throw std::runtime_error("backend returned a null query result");
                }
            } catch (const std::exception &exception) {
                throw std::runtime_error("Insertion failed on backend " + std::to_string(backend_id) + ": " +
                                         exception.what());
            }
        }
    }

    void InsertionCoordinator::record_committed(const std::vector<PlannedEntity> &planned) const {
        for (const PlannedEntity &entity: planned) {
            for (const BackendId backend_id: entity.decision.target_backends) {
                const bool owned = entity.decision.home_backend == backend_id;
                load_tracker_->record_committed(backend_id, entity.item.kind(), 1, owned);
            }
        }
    }

    InsertionReceipt InsertionCoordinator::receipt_for(const PlannedEntity &planned) {
        return InsertionReceipt{.global_id = planned.item.global_id(),
                                .kind = planned.item.kind(),
                                .type = planned.item.type(),
                                .target_backends = planned.decision.target_backends,
                                .home_backend = planned.decision.home_backend};
    }

} // namespace cpgdb::insertion
