/**
 * @file include/events/EventAccess.hpp
 * @brief Process-wide access to the execution-event collector.
 */
#pragma once

#include "events/EventCollector.hpp"
#include "events/EventContext.hpp"
#include "events/EventScope.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace polygraph::events {

[[nodiscard]] EventCollector* collector() noexcept;
[[nodiscard]] EventCollector* collector(EventLevel level, std::string_view event_name) noexcept;
void install(std::unique_ptr<EventCollector> collector) noexcept;
std::unique_ptr<EventCollector> release() noexcept;

[[nodiscard]] inline bool enabled(EventLevel level, std::string_view event_name) noexcept {
    return collector(level, event_name) != nullptr;
}

template <typename NameFactory, typename FieldsFactory>
void emit(EventLevel level, NameFactory&& make_name, FieldsFactory&& make_fields) {
    // Event names are evaluated once. Structured fields are evaluated only when
    // the collector and its runtime filters accept this event.
    auto&& name_value = std::forward<NameFactory>(make_name)();
    const std::string_view name_view{name_value};
    auto* current = collector(level, name_view);
    if (current == nullptr) {
        return;
    }

    auto fields = std::forward<FieldsFactory>(make_fields)();
    if (const auto query_execution_id = current_query_execution_id(); query_execution_id != 0 &&
        !fields.contains("query_execution_id")) {
        fields["query_execution_id"] = query_execution_id;
    }
    current->emit(level, name_view, std::move(fields));
}

template <typename NameFactory, typename FieldsFactory>
[[nodiscard]] EventScope make_scope(EventLevel level,
                                    NameFactory&& make_name,
                                    FieldsFactory&& make_fields) {
    // Preserve string literals/string_views without first materializing std::string.
    // A dynamic name is evaluated once and kept alive for the collector lookup.
    auto&& name_value = std::forward<NameFactory>(make_name)();
    const std::string_view name_view{name_value};
    auto* current = collector(level, name_view);
    if (current == nullptr) {
        return {};
    }

    auto fields = std::forward<FieldsFactory>(make_fields)();
    if (const auto query_execution_id = current_query_execution_id(); query_execution_id != 0 &&
        !fields.contains("query_execution_id")) {
        fields["query_execution_id"] = query_execution_id;
    }

    return EventScope{
        current,
        level,
        std::string{name_view},
        std::move(fields),
    };
}

} // namespace polygraph::events
