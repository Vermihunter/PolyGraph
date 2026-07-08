//
// Created by akos on 8. 7. 2026.
//

#ifndef CPGDB_BFSLAYER_HPP
#define CPGDB_BFSLAYER_HPP

#include <cstdint>
#include <vector>

#include "data_model/NodeRef.hpp"

struct BfsTreeEdgeRef {
    NodeRef source;
    NodeRef target;
};

struct BfsLayerEvent {
    std::uint32_t depth = 0;
    std::vector<NodeRef> vertices;
    std::vector<BfsTreeEdgeRef> tree_edges;
};


#endif //CPGDB_BFSLAYER_HPP
