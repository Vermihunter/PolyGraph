#ifndef PATH_HPP
#define PATH_HPP

#include <vector>
#include <memory>
// #include <data_model/Edge.hpp>
// #include <data_model/Node.hpp>

struct Edge;
struct Node;


struct Path {
  std::vector<std::unique_ptr<Node>> nodes;
  std::vector<std::unique_ptr<Edge>> edges;
};

#endif // PATH_HPP