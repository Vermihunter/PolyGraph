#pragma once

#include <charconv>
#include <cmath>
#include <libpq-fe.h>
#include "data_access/adapters/PostgreSQL/RelationalMapping.hpp"

namespace polygraph::postgres::detail {
    inline std::string sql_text(PGresult *r, int row, int column) {
        if (PQgetisnull(r, row, column))
            throw std::invalid_argument("NULL relational key/value");
        if (PQgetlength(r, row, column) > 1024 * 1024)
            throw std::invalid_argument("Relational field exceeds 1 MiB");
        return {PQgetvalue(r, row, column), static_cast<std::size_t>(PQgetlength(r, row, column))};
    }
    inline json sql_key(PGresult *r, int row, int column, KeyCodec codec) {
        const auto text = sql_text(r, row, column);
        if (codec != KeyCodec::int64)
            return text;
        std::int64_t value{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size())
            throw std::invalid_argument("Invalid native SQL integer key");
        return value;
    }
    inline json sql_property(PGresult *r, int row, int column, domain::PropertyType type) {
        if (PQgetisnull(r, row, column))
            return nullptr;
        auto text = sql_text(r, row, column);
        switch (type) {
            case domain::PropertyType::integer:
                return sql_key(r, row, column, KeyCodec::int64);
            case domain::PropertyType::boolean:
                if (text == "t")
                    return true;
                if (text == "f")
                    return false;
                throw std::invalid_argument("Invalid native SQL boolean");
            case domain::PropertyType::_float: {
                double value{};
                const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
                if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
                    throw std::invalid_argument("SQL floating value cannot be represented as a finite JSON number");
                return value;
            }
            case domain::PropertyType::datetime: {
                const auto separator = text.find(' ');
                if (separator != std::string::npos)
                    text[separator] = 'T';
                if (PQftype(r, column) == 1184 && text.ends_with("+00"))
                    text.replace(text.size() - 3, 3, "Z");
                return text;
            }
            case domain::PropertyType::date:
            case domain::PropertyType::string:
                return text;
        }
        throw std::invalid_argument("Unsupported relational property type");
    }
} // namespace polygraph::postgres::detail
