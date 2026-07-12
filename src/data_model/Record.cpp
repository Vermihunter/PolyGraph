/**
 * @file src/data_model/Record.cpp
 * @brief Implementation for record in the graph data-model module.
 */

#include "data_model/Record.hpp"

namespace cpgdb {

    const Value &Record::operator[](const std::size_t index) const { return fields[index]; }

} // namespace cpgdb
