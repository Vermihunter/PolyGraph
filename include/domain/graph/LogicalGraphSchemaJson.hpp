#pragma once
#include "domain/graph/LogicalGraphSchema.hpp"
#include "utilities/json.hpp"
namespace polygraph::domain {
    [[nodiscard]] graph_t logical_schema_from_json(const json &);
}
