/**
 * @file include/data_model/Path.hpp
 * @brief Declarations for path in the graph data-model module.
 */

#pragma once

#include <memory>
#include <vector>

namespace polygraph {

struct Edge;
struct Node;

struct Path {
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Edge>> edges;
};

} // namespace polygraph
