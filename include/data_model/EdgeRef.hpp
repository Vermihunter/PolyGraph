/**
 * @file include/data_model/EdgeRef.hpp
 * @brief Declarations for edge ref in the graph data-model module.
 */

//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_EDGEREF_HPP
#define CPGDB_EDGEREF_HPP

#include "EntityId.hpp"
#include "domain/graph/TypeId.hpp"

namespace cpgdb {
struct EdgeRef {
    GlobalId global_id;
    TypeId type;
    GlobalId from;
    GlobalId to;
};

} // namespace cpgdb

#endif // CPGDB_EDGEREF_HPP
