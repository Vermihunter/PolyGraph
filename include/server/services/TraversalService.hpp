//
// Created by akos on 7. 7. 2026.
//

#ifndef CPGDB_TRAVERSALSERVICE_HPP
#define CPGDB_TRAVERSALSERVICE_HPP
#include <stdexcept>

#include "algorithms/bfs/BfsResult.hpp"
#include "cli/BfsCli.hpp"
#include "infrastructure/DatabaseContext.hpp"
#include "server/dto/BfsRequest.hpp"


namespace services {


    class TraversalService {
    public:
        explicit TraversalService(DatabaseContext& ctx) : ctx_(ctx) {};

        [[nodiscard]] BfsResult runBfs(
            const std::string& graphId,
            const dto::BfsRequest& request
        );

        BfsResult streamBfs(
           const std::string& graphId,
           const std::string& sourceVertexId,
           const std::string& sourceType,
           std::uint32_t maxDepth,
           bool returnPredecessors,
           bool allowNativePushdown,
           bool allowPartialAcceleration,
           const std::function<void(const BfsLayerEvent&)>& onLayer
       );

    private:
        DatabaseContext& ctx_;
    };
}

#endif //CPGDB_TRAVERSALSERVICE_HPP
