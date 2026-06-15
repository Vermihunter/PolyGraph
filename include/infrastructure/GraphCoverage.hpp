//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_GRAPHCOVERAGE_HPP
#define CPGDB_GRAPHCOVERAGE_HPP
#include <unordered_set>
#include <vector>

#include "algorithms/ExpandSpec.hpp"
#include "data_model/NodeRef.hpp"
#include "domain/graph/TypeId.hpp"

/**
 * What part of the logical graph this backend/projection contains
 */
struct GraphCoverage {
    bool fresh = true;

    // If true, this backend/projection has all node types.
    bool covers_all_node_types = false;

    // If true, this backend/projection has all edge types.
    bool covers_all_edge_types = false;

    // Important:
    // "complete_for_requested_subgraph" means:
    //
    // If a BFS is allowed to traverse Person-LIKES-Post,
    // then this backend has all relevant Person/Post vertices and all LIKES edges
    // needed to answer that BFS without asking another backend.
    bool complete_for_requested_subgraph = false;

    std::unordered_set<TypeId> node_types;
    std::unordered_set<TypeId> edge_types;

    std::uint64_t graph_version = 0;

    [[nodiscard]] bool covers_node_type(TypeId type) const {
        return covers_all_node_types || node_types.contains(type);
    }

    [[nodiscard]] bool covers_edge_type(TypeId type) const {
        return covers_all_edge_types || edge_types.contains(type);
    }

    [[nodiscard]] bool covers_expand_spec(const NodeRef& source,
                                          const ExpandSpec& spec) const {
        if (!fresh) {
            return false;
        }

        if (!covers_node_type(source.type)) {
            return false;
        }

        if (!spec.allowed_edge_types.empty()) {
            for (TypeId edge_type : spec.allowed_edge_types) {
                if (!covers_edge_type(edge_type)) {
                    return false;
                }
            }
        } else if (!covers_all_edge_types) {
            return false;
        }

        if (!spec.allowed_target_node_types.empty()) {
            for (TypeId node_type : spec.allowed_target_node_types) {
                if (!covers_node_type(node_type)) {
                    return false;
                }
            }
        } else if (!covers_all_node_types) {
            return false;
        }

        return true;
    }

    [[nodiscard]] bool can_answer_whole_bfs(const NodeRef& source,
                                            const ExpandSpec& spec) const {
        return complete_for_requested_subgraph &&
               covers_expand_spec(source, spec);
    }
};

#endif //CPGDB_GRAPHCOVERAGE_HPP