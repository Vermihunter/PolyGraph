/**
 * @file src/data_model/Value.cpp
 * @brief Implementation for value in the graph data-model module.
 */

#include "data_model/Value.hpp"

namespace cpgdb {

    bool Value::is_null() const noexcept { return std::holds_alternative<std::monostate>(data); }

} // namespace cpgdb
