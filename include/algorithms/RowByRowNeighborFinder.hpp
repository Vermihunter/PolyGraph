//
// Created by akos on 19. 3. 2026.
//

#ifndef CPGDB_ROWBYROWNEIGHBORFINDER_HPP
#define CPGDB_ROWBYROWNEIGHBORFINDER_HPP

#include <data_access/DatabaseTypes.hpp>
#include <algorithms/NeighborFinder.hpp>

#include <engine/DatabaseContext.hpp>
#include <engine/NodeLocator.hpp>
#include <query/query_types/GetNeighborsForVertexQuery.hpp>

class RowByRowNeighborFinder : public NeighborFinder {
    node_locator_t locator_;

    DatabaseContext& db_context_;
public:
    RowByRowNeighborFinder(DatabaseContext& db_context, node_locator_t locator) :
       db_context_(db_context), locator_(std::move(locator)) {}

    std::vector<node_t> find_neighbors(Node& node) override {
        std::vector<node_t> results;


        auto db = locator_->locate(node.global_id);
        /*
        if (db != DatabaseTypes::NEO4J) {
            return results;
        }
        */

        auto& db_ctx = db_context_.db_registry->get(db);

        auto query_results = db_ctx->queryVisitor->visit(GetNeighborsForVertexQuery( node));

        for (auto&& result : query_results->data) {
            if (!result) {
                std::cout << "No results" << std::endl;
                continue;
            }

            try {
                auto& neighbor = std::get<Node>(result->fields[0].data);
                results.push_back(std::make_unique<Node>(std::move(neighbor)));
            } catch (std::exception& e) {
                std::cout << e.what() << std::endl;
            }
        }

        return results;
    }

};

#endif //CPGDB_ROWBYROWNEIGHBORFINDER_HPP