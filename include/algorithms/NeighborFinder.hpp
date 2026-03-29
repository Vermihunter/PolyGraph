//
// Created by akos on 19. 3. 2026.
//

#ifndef CPGDB_NODEFINDER_HPP
#define CPGDB_NODEFINDER_HPP

#include <vector>
#include <memory>

#include <data_model/Node.hpp>
#include <data_model/Value.hpp>


class NeighborFinder {
public:
    virtual ~NeighborFinder() = default;

    virtual std::vector<node_t> find_neighbors(Node& node) = 0;
};

using neighbor_finder_t = std::unique_ptr<NeighborFinder>;

#endif //CPGDB_NODEFINDER_HPP