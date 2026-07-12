/**
 * @file src/insertion/InsertionItem.cpp
 * @brief Implementation for insertion item in the insertion planning module.
 */

#include "insertion/InsertionItem.hpp"

#include <stdexcept>
#include <utility>

namespace cpgdb::insertion {

    InsertionItem::InsertionItem(std::shared_ptr<const Node> node) : entity_(std::move(node)) {
        if (!std::get<std::shared_ptr<const Node>>(entity_)) {
            throw std::invalid_argument("InsertionItem requires a non-null node");
        }
    }

    InsertionItem::InsertionItem(std::shared_ptr<const Edge> edge) : entity_(std::move(edge)) {
        if (!std::get<std::shared_ptr<const Edge>>(entity_)) {
            throw std::invalid_argument("InsertionItem requires a non-null edge");
        }
    }

    EntityKind InsertionItem::kind() const noexcept { return is_node() ? EntityKind::Node : EntityKind::Edge; }

    bool InsertionItem::is_node() const noexcept {
        return std::holds_alternative<std::shared_ptr<const Node>>(entity_);
    }

    bool InsertionItem::is_edge() const noexcept { return !is_node(); }

    const Node &InsertionItem::node() const {
        if (!is_node()) {
            throw std::logic_error("InsertionItem does not contain a node");
        }
        return *std::get<std::shared_ptr<const Node>>(entity_);
    }

    const Edge &InsertionItem::edge() const {
        if (!is_edge()) {
            throw std::logic_error("InsertionItem does not contain an edge");
        }
        return *std::get<std::shared_ptr<const Edge>>(entity_);
    }

    const GlobalId &InsertionItem::global_id() const noexcept {
        return is_node() ? std::get<std::shared_ptr<const Node>>(entity_)->global_id
                         : std::get<std::shared_ptr<const Edge>>(entity_)->global_id;
    }

    TypeId InsertionItem::type() const noexcept {
        return is_node() ? std::get<std::shared_ptr<const Node>>(entity_)->type
                         : std::get<std::shared_ptr<const Edge>>(entity_)->type;
    }

} // namespace cpgdb::insertion
