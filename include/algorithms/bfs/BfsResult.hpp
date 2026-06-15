//
// Created by akos on 1. 5. 2026.
//

#ifndef CPGDB_BFSRESULT_HPP
#define CPGDB_BFSRESULT_HPP

#include <unordered_map>
#include <boost/container_hash/hash.hpp>

#include "data_model/EntityId.hpp"

struct BfsResult {
    std::unordered_map<GlobalId, std::uint32_t, boost::hash<GlobalId>> distance;
    std::unordered_map<GlobalId, GlobalId, boost::hash<GlobalId>> predecessor;
};

#endif //CPGDB_BFSRESULT_HPP