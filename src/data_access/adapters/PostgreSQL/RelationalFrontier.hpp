#pragma once

#include <algorithm>
#include <unordered_set>
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"
#include "data_access/adapters/PostgreSQL/RelationalMappingValidator.hpp"

namespace polygraph::postgres::detail {
    inline std::string physical_table(const SqlTable &t) {
        return quote_identifier(t.schema) + "." + quote_identifier(t.name);
    }
    inline std::string key_type(KeyCodec codec) {
        switch (codec) {
            case KeyCodec::int64:
                return "pg_catalog.int8";
            case KeyCodec::string:
                return "pg_catalog.text";
            case KeyCodec::uuid:
                return "pg_catalog.uuid";
        }
        throw std::invalid_argument("Unsupported relational key codec");
    }

    inline std::string frontier_sql(KeyCodec codec) {
        return "WITH frontier AS (SELECT (value OPERATOR(pg_catalog.->>) 'ordinal')::pg_catalog.int4 AS ordinal, "
               "(value OPERATOR(pg_catalog.->>) 'key')::" +
               key_type(codec) + " AS key FROM pg_catalog.jsonb_array_elements($1::pg_catalog.jsonb) AS input(value)) ";
    }
    inline std::string pk_expression(const NodeMapping &node, const char *alias,
                                     const ValidatedRelationalMapping &metadata) {
        const auto table = std::ranges::find(metadata.tables(), node.table, &RelationalTableMetadata::table);
        if (table == metadata.tables().end())
            throw std::invalid_argument("Missing validated endpoint metadata");
        auto result = std::string(alias) + "." + quote_identifier(node.primary_key);
        const auto &collation = table->columns.at(node.primary_key).collation;
        if (collation)
            result += " COLLATE " + physical_table(*collation);
        return result;
    }
    inline json frontier_keys(const std::vector<ResolvedRelationalNode> &origins, TypeId type,
                              bool deduplicate = true) {
        json result = json::array();
        std::unordered_set<GlobalId> included;
        for (std::size_t i = 0; i < origins.size(); ++i)
            if (origins[i].node.type == type && (!deduplicate || included.insert(origins[i].node.global_id).second))
                result.push_back({{"ordinal", i}, {"key", origins[i].source.value}});
        return result;
    }
} // namespace polygraph::postgres::detail
