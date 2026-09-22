/**
 * @file include/data_model/EdgeRef.hpp
 * @brief Declarations for edge ref in the graph data-model module.
 */

//
// Created by akos on 5. 5. 2026.
//

#ifndef POLYGRAPH_EDGEREF_HPP
#define POLYGRAPH_EDGEREF_HPP

#include "EntityId.hpp"
#include "domain/graph/TypeId.hpp"

namespace polygraph {
struct EdgeRef {
    GlobalId global_id;
    TypeId type;
    GlobalId from;
    GlobalId to;
};

} // namespace polygraph

#endif // POLYGRAPH_EDGEREF_HPP
