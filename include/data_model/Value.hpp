#ifndef VALUE_HPP
#define VALUE_HPP

#include <unordered_map>
#include <vector>
#include <variant>
#include <string>

#include <data_model/Edge.hpp>
#include <data_model/Node.hpp>
#include <data_model/Path.hpp>

struct Node;
struct Edge;
struct Path;

struct Value;
using value_t = std::unique_ptr<Value>;


using Map  = std::unordered_map<std::string, value_t>;
using List = std::vector<value_t>;

struct Value {
  using Variant = std::variant<
      std::monostate,          // null
      bool,
      int64_t,
      double,
      std::string,
      std::vector<uint8_t>,    // optional
      List,
      Map,
      Node,
      Edge,
      Path>;

  Variant data;

  // convenience
  bool is_null() const { return std::holds_alternative<std::monostate>(data); }
  template<class T> bool is() const { return std::holds_alternative<T>(data); }
  template<class T> const T& as() const { return std::get<T>(data); }
};

#endif // VALUE_HPP