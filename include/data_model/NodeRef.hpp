//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_NODEREF_HPP
#define CPGDB_NODEREF_HPP

#include "EntityId.hpp"
#include "domain/graph/TypeId.hpp"

struct NodeRef {
    GlobalId global_id;
    TypeId type;
};

#endif //CPGDB_NODEREF_HPP