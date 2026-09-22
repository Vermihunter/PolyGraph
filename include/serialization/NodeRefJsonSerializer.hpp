//
// Created by akos on 11. 7. 2026.
//

#ifndef POLYGRAPH_NODEREFJSONSERIALIZER_HPP
#define POLYGRAPH_NODEREFJSONSERIALIZER_HPP
#include "data_model/NodeRef.hpp"
#include "utilities/json.hpp"

namespace polygraph {

void to_json(json& value, const NodeRef& node);
}

#endif // POLYGRAPH_NODEREFJSONSERIALIZER_HPP
