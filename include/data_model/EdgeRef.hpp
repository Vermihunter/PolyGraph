//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_EDGEREF_HPP
#define CPGDB_EDGEREF_HPP

#include "EntityId.hpp"
#include "domain/graph/TypeId.hpp"

struct EdgeRef {
    GlobalId global_id;
    TypeId type;
    GlobalId from;
    GlobalId to;
};

#endif //CPGDB_EDGEREF_HPP