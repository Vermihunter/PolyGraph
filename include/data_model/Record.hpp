/**
 * @file include/data_model/Record.hpp
 * @brief Declarations for record in the graph data-model module.
 */

#pragma once

#include "data_model/Value.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace polygraph {

struct Record {
    std::vector<Value> fields;

    Record() = default;
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;
    Record(Record&&) = default;
    Record& operator=(Record&&) = default;

    [[nodiscard]] const Value& operator[](std::size_t index) const;
};

using record_t = std::unique_ptr<Record>;

} // namespace polygraph
