/**
 * @file include/insertion/InsertionItem.hpp
 * @brief Immutable shared ownership wrapper for a node or edge write payload.
 */

#ifndef POLYGRAPH_INSERTION_INSERTION_ITEM_HPP
#define POLYGRAPH_INSERTION_INSERTION_ITEM_HPP

#include "data_model/Value.hpp"

#include <memory>
#include <variant>

namespace polygraph::insertion {

/** @brief Distinguishes node and edge insertion payloads without inspecting variants. */
enum class EntityKind { Node, Edge };

/**
 * @brief Type-safe immutable node-or-edge payload used by insertion queries.
 *
 * Shared immutable ownership makes query cloning inexpensive and permits the same
 * payload to be dispatched to multiple replica backends without deep-copying
 * move-only property values.
 */
class InsertionItem {
  public:
    /** @brief Wraps a non-null immutable node. */
    explicit InsertionItem(std::shared_ptr<const Node> node);
    /** @brief Wraps a non-null immutable edge. */
    explicit InsertionItem(std::shared_ptr<const Edge> edge);

    /** @brief Returns the active entity category. */
    [[nodiscard]] EntityKind kind() const noexcept;
    /** @brief Returns true when this item contains a node. */
    [[nodiscard]] bool is_node() const noexcept;
    /** @brief Returns true when this item contains an edge. */
    [[nodiscard]] bool is_edge() const noexcept;
    /** @brief Returns the contained node or throws std::bad_variant_access. */
    [[nodiscard]] const Node& node() const;
    /** @brief Returns the contained edge or throws std::bad_variant_access. */
    [[nodiscard]] const Edge& edge() const;
    /** @brief Returns the stable ID of the contained entity. */
    [[nodiscard]] const GlobalId& global_id() const noexcept;
    /** @brief Returns the logical node or edge type ID. */
    [[nodiscard]] TypeId type() const noexcept;

  private:
    using Storage = std::variant<std::shared_ptr<const Node>, std::shared_ptr<const Edge>>;
    Storage entity_;
};

} // namespace polygraph::insertion

#endif // POLYGRAPH_INSERTION_INSERTION_ITEM_HPP
