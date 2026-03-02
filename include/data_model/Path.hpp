#ifndef PATH_HPP
#define PATH_HPP

#include <vector>

#include <data_model/Edge.hpp>
#include <data_model/Node.hpp>

struct Path {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
};

#endif // PATH_HPP