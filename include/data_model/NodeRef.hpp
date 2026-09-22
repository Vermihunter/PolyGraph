/**
 * @file include/data_model/NodeRef.hpp
 * @brief Declarations for node ref in the graph data-model module.
 */

//
// Created by akos on 5. 5. 2026.
//

#ifndef POLYGRAPH_NODEREF_HPP
#define POLYGRAPH_NODEREF_HPP

#include "EntityId.hpp"
#include "domain/graph/TypeId.hpp"

namespace polygraph {
struct NodeRef {
    GlobalId global_id;
    // An ID-only API entry point leaves this unresolved. Storage adapters
    // return concrete logical types for discovered neighbors.
    TypeId type = unresolved_type_id;
};

} // namespace polygraph

#endif // POLYGRAPH_NODEREF_HPP
