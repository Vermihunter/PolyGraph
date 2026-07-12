/**
 * @file include/data_model/NodeRef.hpp
 * @brief Declarations for node ref in the graph data-model module.
 */

//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_NODEREF_HPP
#define CPGDB_NODEREF_HPP

#include "EntityId.hpp"
#include "domain/graph/TypeId.hpp"

namespace cpgdb {
struct NodeRef {
    GlobalId global_id;
    TypeId type;
};

} // namespace cpgdb

#endif // CPGDB_NODEREF_HPP
