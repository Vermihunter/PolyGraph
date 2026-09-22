/**
 * @file include/data_model/Value.hpp
 * @brief Declarations for value in the graph data-model module.
 */

#pragma once

#include "data_model/Edge.hpp"
#include "data_model/Node.hpp"
#include "data_model/Path.hpp"

#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace polygraph {

struct Value;
using value_t = std::unique_ptr<Value>;
using Map = std::unordered_map<std::string, value_t>;
using List = std::vector<value_t>;

struct Value {
    using Variant = std::variant<std::monostate,
                                 bool,
                                 std::int64_t,
                                 double,
                                 std::string,
                                 std::vector<std::uint8_t>,
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

    template <typename T>
        requires(!std::same_as<std::remove_cvref_t<T>, Value>)
    Value(T&& value)
        : data(std::forward<T>(value)) {}

    [[nodiscard]] bool is_null() const noexcept;

    template <typename T> [[nodiscard]] bool is() const noexcept {
        return std::holds_alternative<T>(data);
    }

    template <typename T> [[nodiscard]] const T& as() const {
        return std::get<T>(data);
    }
};

} // namespace polygraph
