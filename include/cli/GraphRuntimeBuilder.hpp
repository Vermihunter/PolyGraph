//
// Created by akos on 7. 7. 2026.
//

#ifndef CPGDB_GRAPHRUNTIMEBUILDER_HPP
#define CPGDB_GRAPHRUNTIMEBUILDER_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "infrastructure/DatabaseContext.hpp"

namespace cpgdb::cli {

    struct GraphRuntimeConfig {
        std::string logical_graph_schema_path;
        std::string data_path;
        std::optional<std::string> backend_config_path;
        std::uint64_t graph_version = 1;
        bool verbose = false;
    };

    class GraphRuntimeBuilder {
    public:
        [[nodiscard]] std::shared_ptr<DatabaseContext> build(const GraphRuntimeConfig& config) const;
    };

} // namespace cpgdb::runtime


#endif //CPGDB_GRAPHRUNTIMEBUILDER_HPP
