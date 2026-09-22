/**
 * @file include/infrastructure/GraphCoverage.hpp
 * @brief Declarations for graph coverage in the backend infrastructure module.
 */

#ifndef POLYGRAPH_INFRASTRUCTURE_GRAPH_COVERAGE_HPP
#define POLYGRAPH_INFRASTRUCTURE_GRAPH_COVERAGE_HPP

#include "algorithms/ExpandSpec.hpp"
#include "data_model/NodeRef.hpp"

#include <cstdint>
#include <unordered_set>

namespace polygraph {

struct GraphCoverage {
    bool fresh = true;
    bool covers_all_node_types = false;
    bool covers_all_edge_types = false;
    bool complete_for_requested_subgraph = false;
    std::unordered_set<TypeId> node_types;
    std::unordered_set<TypeId> edge_types;
    std::uint64_t graph_version = 0;

    [[nodiscard]] bool covers_node_type(TypeId type) const noexcept;
    [[nodiscard]] bool covers_edge_type(TypeId type) const noexcept;
    [[nodiscard]] bool covers_expand_spec(const NodeRef& source, const ExpandSpec& spec) const;
    [[nodiscard]] bool can_answer_whole_bfs(const NodeRef& source, const ExpandSpec& spec) const;
};

} // namespace polygraph

#endif // POLYGRAPH_INFRASTRUCTURE_GRAPH_COVERAGE_HPP
