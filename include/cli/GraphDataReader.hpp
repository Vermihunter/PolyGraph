/**
 * @file include/cli/GraphDataReader.hpp
 * @brief Backward-compatible reader for monolithic JSON and line-oriented graph data.
 */

#pragma once

#include "utilities/json.hpp"

#include <functional>
#include <string>

namespace polygraph::cli {

/**
 * Visits graph records without requiring the whole data file to be represented by a
 * nlohmann::json DOM. JSONL files must contain all node records before relationship records.
 */
class GraphDataReader {
  public:
    using RecordVisitor = std::function<void(const json&)>;

    [[nodiscard]] static json metadata(const std::string& path);

    static void for_each(const std::string& path, const RecordVisitor& on_node,
                         const RecordVisitor& on_relationship);
};

} // namespace polygraph::cli
