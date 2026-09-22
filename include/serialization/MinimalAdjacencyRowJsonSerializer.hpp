//
// Created by akos on 11. 7. 2026.
//

#ifndef POLYGRAPH_MINIMALADJACENCYROWJSONSERIALIZER_HPP
#define POLYGRAPH_MINIMALADJACENCYROWJSONSERIALIZER_HPP
#include "data_model/MinimalAdjacencyRow.hpp"
#include "utilities/json.hpp"

namespace polygraph {

void to_json(json& value, const MinimalAdjacencyRow& adj_row);
// void from_json(const json& value, MinimalAdjacencyRow& node);
} // namespace polygraph

#endif // POLYGRAPH_MINIMALADJACENCYROWJSONSERIALIZER_HPP
