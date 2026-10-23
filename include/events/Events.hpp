/**
 * @file include/events/Events.hpp
 * @brief Compile-time removable structured execution-event instrumentation.
 *
 * POLYGRAPH_ENABLE_EVENT_GATHERING=0 makes every EVENT_* macro a preprocessing
 * no-op. Arguments are not evaluated and no event-scope object is created.
 */
#pragma once

#define POLYGRAPH_EVENT_LEVEL_TRACE 0
#define POLYGRAPH_EVENT_LEVEL_DEBUG 1
#define POLYGRAPH_EVENT_LEVEL_INFO 2
#define POLYGRAPH_EVENT_LEVEL_WARN 3
#define POLYGRAPH_EVENT_LEVEL_ERROR 4
#define POLYGRAPH_EVENT_LEVEL_OFF 5

#ifndef POLYGRAPH_ENABLE_EVENT_GATHERING
#define POLYGRAPH_ENABLE_EVENT_GATHERING 1
#endif

#ifndef POLYGRAPH_EVENT_COMPILED_LEVEL
#define POLYGRAPH_EVENT_COMPILED_LEVEL POLYGRAPH_EVENT_LEVEL_INFO
#endif

#if POLYGRAPH_ENABLE_EVENT_GATHERING

#include "events/EventAccess.hpp"
#include "events/EventLevel.hpp"
#include "utilities/json.hpp"

#define POLYGRAPH_EVENT_IMPL(level, name, ...)                                                \
    ::polygraph::events::emit(                                                                \
        (level),                                                                              \
        [&]() -> decltype(auto) { return (name); },                                           \
        [&]() -> nlohmann::json { return nlohmann::json{__VA_ARGS__}; })

#define POLYGRAPH_EVENT_SCOPE_IMPL(var_name, level, name, ...)                               \
    [[maybe_unused]] auto var_name = ::polygraph::events::make_scope(                         \
        (level),                                                                              \
        [&]() -> decltype(auto) { return (name); },                                           \
        [&]() -> nlohmann::json { return nlohmann::json{__VA_ARGS__}; })

#define POLYGRAPH_EVENT_SCOPE_FIELD_IMPL(var_name, key, value) \
    do {                                                      \
        if ((var_name).active()) {                            \
            (var_name).add_end_field((key), (value));         \
        }                                                     \
    } while (false)
#define EVENT_ENABLED(level, name) (::polygraph::events::enabled((level), (name)))

#else

#define POLYGRAPH_EVENT_IMPL(...) ((void)0)
#define POLYGRAPH_EVENT_SCOPE_IMPL(...) ((void)0)
#define POLYGRAPH_EVENT_SCOPE_FIELD_IMPL(...) ((void)0)
#define EVENT_ENABLED(...) false

#endif

#if POLYGRAPH_ENABLE_EVENT_GATHERING && POLYGRAPH_EVENT_COMPILED_LEVEL <= POLYGRAPH_EVENT_LEVEL_TRACE
#define EVENT_TRACE(name, ...) POLYGRAPH_EVENT_IMPL(::polygraph::EventLevel::Trace, name __VA_OPT__(,) __VA_ARGS__)
#define EVENT_SCOPE_TRACE(var_name, name, ...)                                                     \
    POLYGRAPH_EVENT_SCOPE_IMPL(var_name, ::polygraph::EventLevel::Trace, name __VA_OPT__(,) __VA_ARGS__)
#define EVENT_SCOPE_TRACE_FIELD(var_name, key, value) POLYGRAPH_EVENT_SCOPE_FIELD_IMPL(var_name, key, value)
#else
#define EVENT_TRACE(...) ((void)0)
#define EVENT_SCOPE_TRACE(...) ((void)0)
#define EVENT_SCOPE_TRACE_FIELD(...) ((void)0)
#endif

#if POLYGRAPH_ENABLE_EVENT_GATHERING && POLYGRAPH_EVENT_COMPILED_LEVEL <= POLYGRAPH_EVENT_LEVEL_DEBUG
#define EVENT_DEBUG(name, ...) POLYGRAPH_EVENT_IMPL(::polygraph::EventLevel::Debug, name __VA_OPT__(,) __VA_ARGS__)
#define EVENT_SCOPE_DEBUG(var_name, name, ...)                                                     \
    POLYGRAPH_EVENT_SCOPE_IMPL(var_name, ::polygraph::EventLevel::Debug, name __VA_OPT__(,) __VA_ARGS__)
#define EVENT_SCOPE_DEBUG_FIELD(var_name, key, value) POLYGRAPH_EVENT_SCOPE_FIELD_IMPL(var_name, key, value)
#else
#define EVENT_DEBUG(...) ((void)0)
#define EVENT_SCOPE_DEBUG(...) ((void)0)
#define EVENT_SCOPE_DEBUG_FIELD(...) ((void)0)
#endif

#if POLYGRAPH_ENABLE_EVENT_GATHERING && POLYGRAPH_EVENT_COMPILED_LEVEL <= POLYGRAPH_EVENT_LEVEL_INFO
#define EVENT_INFO(name, ...) POLYGRAPH_EVENT_IMPL(::polygraph::EventLevel::Info, name __VA_OPT__(,) __VA_ARGS__)
#define EVENT_SCOPE_INFO(var_name, name, ...)                                                      \
    POLYGRAPH_EVENT_SCOPE_IMPL(var_name, ::polygraph::EventLevel::Info, name __VA_OPT__(,) __VA_ARGS__)
#define EVENT_SCOPE_INFO_FIELD(var_name, key, value) POLYGRAPH_EVENT_SCOPE_FIELD_IMPL(var_name, key, value)
#else
#define EVENT_INFO(...) ((void)0)
#define EVENT_SCOPE_INFO(...) ((void)0)
#define EVENT_SCOPE_INFO_FIELD(...) ((void)0)
#endif

#if POLYGRAPH_ENABLE_EVENT_GATHERING && POLYGRAPH_EVENT_COMPILED_LEVEL <= POLYGRAPH_EVENT_LEVEL_WARN
#define EVENT_WARN(name, ...) POLYGRAPH_EVENT_IMPL(::polygraph::EventLevel::Warn, name __VA_OPT__(,) __VA_ARGS__)
#else
#define EVENT_WARN(...) ((void)0)
#endif

#if POLYGRAPH_ENABLE_EVENT_GATHERING && POLYGRAPH_EVENT_COMPILED_LEVEL <= POLYGRAPH_EVENT_LEVEL_ERROR
#define EVENT_ERROR(name, ...) POLYGRAPH_EVENT_IMPL(::polygraph::EventLevel::Error, name __VA_OPT__(,) __VA_ARGS__)
#else
#define EVENT_ERROR(...) ((void)0)
#endif
