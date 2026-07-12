/**
 * @file include/infrastructure/ConfiguredGraphStoreFactory.hpp
 * @brief Declarations for configured graph store factory in the backend infrastructure module.
 */

#ifndef CPGDB_INFRASTRUCTURE_CONFIGURED_GRAPH_STORE_FACTORY_HPP
#define CPGDB_INFRASTRUCTURE_CONFIGURED_GRAPH_STORE_FACTORY_HPP

#include "data_access/IGraphStore.hpp"
#include "data_model/Record.hpp"
#include "domain/graph/LogicalGraphSchema.hpp"
#include "gpu/data_model/HostAdjacencyIndex.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/BackendDescriptor.hpp"
#include "infrastructure/BackendId.hpp"
#include "utilities/json.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cpgdb {

class DatabaseContext;

class ConfiguredGraphStoreFactory {
  public:
    struct GpuProjectionInput {
        std::vector<record_t> gpu_records;
        std::shared_ptr<HostAdjacencyIndex> host_index;
    };

    struct Inputs {
        std::string data_path;
        const domain::LogicalGraphSchema& schema;
        const std::unordered_map<BackendId, std::unordered_set<TypeId>>& nodes_by_backend;
        const std::unordered_map<BackendId, std::unordered_set<TypeId>>& edges_by_backend;
        std::uint64_t graph_version = 0;
        std::function<GpuProjectionInput(const std::string&,
                                         const std::shared_ptr<domain::LogicalGraphSchema>&)>
            build_gpu_projection_inputs;
        std::function<void()> ensure_cuda;
    };

    [[nodiscard]] std::shared_ptr<IGraphStore> create_from_json(DatabaseContext& context,
                                                                const json& backend_json,
                                                                const Inputs& inputs) const;

  private:
    [[nodiscard]] static std::shared_ptr<IGraphStore>
    create_neo4j(DatabaseContext& context,
                 const BackendDescriptor& descriptor,
                 const BackendConnectionConfig& connection);
    [[nodiscard]] static std::shared_ptr<IGraphStore>
    create_arango(DatabaseContext& context,
                  const BackendDescriptor& descriptor,
                  const BackendConnectionConfig& connection);
    [[nodiscard]] static std::shared_ptr<IGraphStore> create_cugraph(
        DatabaseContext& context, const BackendDescriptor& descriptor, const Inputs& inputs);
};

} // namespace cpgdb

#endif // CPGDB_INFRASTRUCTURE_CONFIGURED_GRAPH_STORE_FACTORY_HPP
