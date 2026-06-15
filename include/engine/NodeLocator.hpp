//
// Created by akos on 19. 3. 2026.
//

#ifndef CPGDB_NODELOCATOR_HPP
#define CPGDB_NODELOCATOR_HPP


#include <utilities/uuid.hpp>
#include "../infrastructure/BackendKind.hpp"
#include <engine/GraphPartitioner.hpp>

class NodeLocator {
    graph_partitioner_t graph_partitioner_;
public:
    NodeLocator(graph_partitioner_t graph_partitioner) :
        graph_partitioner_(std::move(graph_partitioner)) {}

    BackendKind locate(GlobalId id) {
        for (auto&& [range, db_type] : *graph_partitioner_) {
            auto&& [id_range_low, id_range_high] = range;

            if (id >= id_range_low && id <= id_range_high) {
                return db_type;
            }
        }

        throw std::invalid_argument("ID range " + boost::lexical_cast<std::string>(id) +  " not found");
    }
};

using node_locator_t = std::unique_ptr<NodeLocator>;

#endif //CPGDB_NODELOCATOR_HPP