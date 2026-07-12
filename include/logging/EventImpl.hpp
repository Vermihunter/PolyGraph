//
// Created by akos on 11. 7. 2026.
//

#ifndef CPGDB_EVENTIMPL_HPP
#define CPGDB_EVENTIMPL_HPP

#define CPGDB_EVENT_IMPL(level, name, ...)                                                         \
    do {                                                                                           \
        if (auto* cpgdb_event_logger_ = ::cpgdb::event_logger((level))) {                          \
            cpgdb_event_logger_->event((level), (name), nlohmann::json{__VA_ARGS__});              \
        }                                                                                          \
    } while (false)

#define CPGDB_EVENT_SCOPE_IMPL(var_name, level, name, ...)                                         \
    [[maybe_unused]] auto var_name =                                                               \
        ::cpgdb::make_event_scope((level),                                                         \
                                  [&]() -> std::string { return std::string{name}; },              \
                                  [&]() -> nlohmann::json { return nlohmann::json{__VA_ARGS__}; })

#define CPGDB_EVENT_SCOPE_FIELD_IMPL(var_name, key, value) (var_name).add_end_field((key), (value))

#endif // CPGDB_EVENTIMPL_HPP
