//
// Created by akos on 11. 7. 2026.
//

#ifndef CPGDB_NODEREFJSONSERIALIZER_HPP
#define CPGDB_NODEREFJSONSERIALIZER_HPP
#include "data_model/NodeRef.hpp"
#include "utilities/json.hpp"

namespace cpgdb {

void to_json(json& value, const NodeRef& node);
}

#endif // CPGDB_NODEREFJSONSERIALIZER_HPP
