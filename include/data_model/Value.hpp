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

/*
struct Map : std::unordered_map<std::string, value_t> {
    using Base = std::unordered_map<std::string, value_t>;
    using Base::Base;

    Map() = default;
    Map(const Map&) = delete;
    Map& operator=(const Map&) = delete;
    Map(Map&&) = default;
    Map& operator=(Map&&) = default;
};
struct List : std::vector<value_t> {
    using Base = std::vector<value_t>;
    using Base::Base;

    List() = default;
    List(const List&) = delete;
    List& operator=(const List&) = delete;
    List(List&&) = default;
    List& operator=(List&&) = default;
};
*/

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

    Value() = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    Value(Value&&) = default;
    Value& operator=(Value&&) = default;

    template<typename T>
      requires (!std::same_as<std::remove_cvref_t<T>, Value>)
    Value(T&& v) : data(std::forward<T>(v)) {}

    // convenience
    bool is_null() const { return std::holds_alternative<std::monostate>(data); }
    template<class T> bool is() const { return std::holds_alternative<T>(data); }
    template<class T> const T& as() const { return std::get<T>(data); }
};

#endif // VALUE_HPP