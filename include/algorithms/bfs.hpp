//
// Created by akos on 19. 3. 2026.
//

#ifndef CPGDB_BFS_HPP
#define CPGDB_BFS_HPP

#include <unordered_map>
#include <queue>
#include <chrono>
#include <iostream>

#include <boost/functional/hash.hpp>

#include <data_model/Node.hpp>
#include <data_model/Value.hpp>
#include <algorithms/BfsOptions.hpp>
#include <algorithms/NeighborFinder.hpp>
#include <utilities/uuid.hpp>

class BFS {
    NeighborFinder& neighborFinder_;
public:
    explicit BFS(NeighborFinder& neighborFinder)
        : neighborFinder_(neighborFinder){}

    [[nodiscard]] auto run(node_t source, std::optional<BfsOptions> options) {

        std::unordered_map<GlobalId, std::size_t, boost::hash<GlobalId>> results {
            {source->global_id, 0}
        };


        std::queue<node_t> queue;
        queue.push(std::move(source));
        queue.push(node_t{});

        size_t depth = 1;
        while (!queue.empty()) {
            node_t node = std::move(queue.front());
            queue.pop();

            // Separating layers
            if (!node) {
                // Adding layer separating empty node pointer if the queue is not empty (protects infinite loop)
                if (!queue.empty()) {
                    queue.push(node_t{});
                }

                // Early exit if specified
                if (
                    options.has_value() &&
                    options.value().maxDepth.has_value() &&
                    options.value().maxDepth.value() <= depth
                ) {
                    break;
                }

                ++depth;
                continue;
            }


            auto neighbor_find_start = std::chrono::high_resolution_clock::now();
            auto neighbors = neighborFinder_.find_neighbors(*node);
            auto neighbor_find_end = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(neighbor_find_end - neighbor_find_start);
            std::cout << "Neighbor Find request for node " << node->global_id << " has resulted in " << neighbors.size() << " neighbors in time " << elapsed.count() << " ms" << std::endl;

            for (auto&& neighbor : neighbors) {
                auto [it, inserted] = results.insert({
                    neighbor->global_id, depth
                });

                if (inserted) {
                    queue.push(std::move(neighbor));
                }
            }
         }

         return results;

    }
};

#endif //CPGDB_BFS_HPP