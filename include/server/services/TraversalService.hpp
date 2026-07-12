/**
 * @file include/server/services/TraversalService.hpp
 * @brief Declarations for traversal service in the server frontend module.
 */

#ifndef CPGDB_SERVER_SERVICES_TRAVERSAL_SERVICE_HPP
#define CPGDB_SERVER_SERVICES_TRAVERSAL_SERVICE_HPP

#include "algorithms/bfs/BfsLayer.hpp"
#include "algorithms/bfs/BfsResult.hpp"
#include "algorithms/common/NeighborExpander.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "server/dto/BfsRequest.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace cpgdb::services {

class TraversalService {
  public:
    explicit TraversalService(DatabaseContext& context) noexcept;

    [[nodiscard]] BfsResult runBfs(const std::string& source_vertex_uuid, const dto::BfsRequest& request);

    [[nodiscard]] std::vector<MinimalAdjacencyRow>
    getNeighbors(const std::string& source_vertex_uuid, TypeId source_vertex_type);

    [[nodiscard]] BfsResult streamBfs(const std::string& graph_id,
                                      const std::string& source_vertex_id,
                                      const std::string& source_type,
                                      std::uint32_t max_depth,
                                      bool return_predecessors,
                                      bool allow_native_pushdown,
                                      bool allow_partial_acceleration,
                                      const std::function<void(const BfsLayerEvent&)>& on_layer);

  private:
    DatabaseContext& context_;
    NeighborExpander expander_;
};

} // namespace cpgdb::services

#endif // CPGDB_SERVER_SERVICES_TRAVERSAL_SERVICE_HPP
