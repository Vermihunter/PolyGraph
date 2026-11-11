#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "domain/graph/LogicalGraphSchema.hpp"
#include "utilities/json.hpp"

namespace polygraph::postgres {
    enum class KeyCodec { int64, string, uuid };
    enum class RelationshipKind { table, foreign_key };
    enum class ForeignKeyOwner { from, to };
    struct SqlTable {
        std::string schema;
        std::string name;
        bool operator==(const SqlTable &) const = default;
    };
    struct PropertyMapping {
        std::string property;
        std::string column;
        domain::PropertyType type;
    };
    struct NodeMapping {
        TypeId type;
        std::string logical_type;
        SqlTable table;
        std::string primary_key;
        std::string name_space;
        KeyCodec key_codec;
        std::vector<PropertyMapping> properties;
    };
    struct RelationshipMapping {
        TypeId type;
        std::string logical_type;
        TypeId from_type;
        TypeId to_type;
        RelationshipKind kind;
        // Used only by table relationships.
        SqlTable table;
        std::string from_column;
        std::string to_column;
        std::optional<std::string> primary_key;
        std::optional<KeyCodec> key_codec;
        std::vector<PropertyMapping> properties;
        // Used only by foreign-key relationships. Graph direction remains logical.
        ForeignKeyOwner owner{ForeignKeyOwner::from};
        std::string column;
    };
    class RelationalMapping {
    public:
        [[nodiscard]] const std::vector<NodeMapping> &nodes() const noexcept { return nodes_; }
        [[nodiscard]] const std::vector<RelationshipMapping> &relationships() const noexcept { return relationships_; }
        [[nodiscard]] const NodeMapping &node(TypeId type) const;

    private:
        std::vector<NodeMapping> nodes_;
        std::vector<RelationshipMapping> relationships_;
        friend RelationalMapping parse_relational_mapping(const json &, const domain::LogicalGraphSchema &);
    };

    [[nodiscard]] RelationalMapping parse_relational_mapping(const json &, const domain::LogicalGraphSchema &);
    [[nodiscard]] RelationalMapping load_relational_mapping(const std::filesystem::path &,
                                                            const domain::LogicalGraphSchema &);
    // References are resolved against the containing configuration, never the process cwd.
    [[nodiscard]] std::optional<std::filesystem::path>
    relational_mapping_path(const json &config, const std::filesystem::path &containing_file);
    [[nodiscard]] std::string quote_identifier(std::string_view identifier);
} // namespace polygraph::postgres
