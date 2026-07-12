//
// Created by akos on 11. 7. 2026.
//

#ifndef CPGDB_MINIMALADJACENCYROWJSONSERIALIZER_HPP
#define CPGDB_MINIMALADJACENCYROWJSONSERIALIZER_HPP
#include "data_model/MinimalAdjacencyRow.hpp"
#include "utilities/json.hpp"

namespace cpgdb {

void to_json(json& value, const MinimalAdjacencyRow& adj_row);
// void from_json(const json& value, MinimalAdjacencyRow& node);
} // namespace cpgdb

#endif // CPGDB_MINIMALADJACENCYROWJSONSERIALIZER_HPP
