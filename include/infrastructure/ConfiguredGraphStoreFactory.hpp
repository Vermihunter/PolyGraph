/**
 * @file include/infrastructure/ConfiguredGraphStoreFactory.hpp
 * @brief Declarations for configured graph store factory in the backend infrastructure module.
 */

#ifndef POLYGRAPH_INFRASTRUCTURE_CONFIGURED_GRAPH_STORE_FACTORY_HPP
#define POLYGRAPH_INFRASTRUCTURE_CONFIGURED_GRAPH_STORE_FACTORY_HPP

#include "data_access/IGraphStore.hpp"
#include "data_model/EntityId.hpp"
#include "domain/graph/LogicalGraphSchema.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"
#include "infrastructure/BackendDescriptor.hpp"
#include "infrastructure/BackendId.hpp"
#include "utilities/json.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace polygraph {

class DatabaseContext;

class ConfiguredGraphStoreFactory {
  public:
    struct GpuProjectionInput {
        // Legacy/global input used only when a cache plan predates reusable GPU
        // projection artifacts. New cache plans provide an already dense, sorted
        // and deduplicated edge list so runtime startup does no graph parsing or
        // GlobalId renumbering.
        std::vector<GlobalId> vertex_ids;
        std::vector<std::pair<GlobalId, GlobalId>> edges;
        std::vector<std::int32_t> dense_sources;
        std::vector<std::int32_t> dense_destinations;
        std::size_t resident_vertex_count{0};
        bool prebuilt_dense{false};

        [[nodiscard]] std::size_t adjacency_pair_count() const noexcept {
            return prebuilt_dense ? dense_sources.size() : edges.size();
        }
    };

    struct Inputs {
        std::string data_path;
        const domain::LogicalGraphSchema& schema;
        const std::unordered_map<BackendId, std::unordered_set<TypeId>>& nodes_by_backend;
        const std::unordered_map<BackendId, std::unordered_set<TypeId>>& edges_by_backend;
        std::uint64_t graph_version = 0;
        std::function<GpuProjectionInput(const std::string&, unsigned int)> build_gpu_projection_inputs;
        std::function<void(unsigned int)> ensure_cuda;
    };

    [[nodiscard]] static std::shared_ptr<IGraphStore> create_from_json(DatabaseContext& context,
                                                                const json& backend_json,
                                                                const Inputs& inputs);

  private:
    [[nodiscard]] static std::shared_ptr<IGraphStore>
    create_neo4j(DatabaseContext& context,
                 const BackendDescriptor& descriptor,
                 const BackendConnectionConfig& connection,
                 const json& backend_json);
    [[nodiscard]] static std::shared_ptr<IGraphStore>
    create_arango(DatabaseContext& context,
                  const BackendDescriptor& descriptor,
                  const BackendConnectionConfig& connection,
                  const json& backend_json);
    [[nodiscard]] static std::shared_ptr<IGraphStore>
    create_postgresql(const BackendDescriptor& descriptor,
                      const BackendConnectionConfig& connection,
                      const domain::LogicalGraphSchema& schema,
                      const json& backend_json);
    [[nodiscard]] static std::shared_ptr<IGraphStore> create_cugraph(
        DatabaseContext& context, const BackendDescriptor& descriptor, const json& backend_json, const Inputs& inputs);
    [[nodiscard]] static std::shared_ptr<IGraphStore> create_redis(
        const BackendDescriptor& descriptor, const BackendConnectionConfig& connection, const json& backend_json);
    [[nodiscard]] static std::shared_ptr<IGraphStore> create_mmap(
        const BackendDescriptor& descriptor, const json& backend_json, const Inputs& inputs);
};

} // namespace polygraph

#endif // POLYGRAPH_INFRASTRUCTURE_CONFIGURED_GRAPH_STORE_FACTORY_HPP
