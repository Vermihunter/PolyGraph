//
// Created by akos on 29. 3. 2026.
//

#ifndef CPGDB_GRAPH_HPP
#define CPGDB_GRAPH_HPP
#include <vector>

#include "Node.hpp"
#include "Edge.hpp"

namespace domain {
    class Graph {
        std::vector<Node> nodes;
        std::vector<Edge> edges;


    public:

        void add_node(Node&& node) {
            nodes.push_back(std::move(node));
        }

        void add_edge(Edge&& edge) {
            edges.push_back(std::move(edge));
        }

        [[nodiscard]] const std::vector<Node>& get_nodes() const {
            return nodes;
        }

        std::vector<Node>& get_nodes() {
            return nodes;
        }

        [[nodiscard]] const std::vector<Edge>& get_edges() const {
            return edges;
        }
    };

    using graph_t = std::unique_ptr<Graph>;
}

#endif //CPGDB_GRAPH_HPP