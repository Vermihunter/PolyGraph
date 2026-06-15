#ifndef ENGINE_HPP
#define ENGINE_HPP

#include <query/QueryResult.hpp>
//#include <query/Query.hpp>

#include <query/QueryContext.hpp>

#include "../infrastructure/DatabaseContext.hpp"
#include "gpu/DataModelToGPUGraphConverter.hpp"
#include "pipeline/QueryExecutionTask.hpp"
#include "pipeline/QueryResultJoinTask.hpp"
#include "pipeline/DataToGPUTask.hpp"
#include "pipeline/GPUPageRanksTask.hpp"
#include "pipeline/Scheduler.hpp"

#include "query/query_types/GetAllVerticesQuery.hpp"

#include "data_access/adapters/cuGraph/cuGraphExample.hpp"
#include "pipeline/GPUConnectedComponentsTask.hpp"
#include "pipeline/PageRanksResultCopyBackTask.hpp"


using vertex_t = int32_t;
using edge_t = int32_t;
using weight_t = float;
using result_t = float;
constexpr bool STORE_TRANSPOSED = false;
constexpr bool MULTI_GPU = false;

// PageRanks -> result_t
// BFS -> vertex_t
using res_t = vertex_t;

class Engine {
    db_context_t ctx;
public:
    Engine(db_context_t _ctx) : ctx(std::move(_ctx)) {}

    query_result_t evaluate(QueryContext&& query_ctx) {
        std::cout << "Evaluating in engine" << std::endl;
        Scheduler scheduler;

        auto load_nodes_neo_local = scheduler.emplace([&]() {
            return execute_query(
                *query_ctx.query,
                *ctx->db_registry->get(BackendKind::NEO4J)->queryVisitor
            );
        });

        auto load_nodes_neo_cloud = scheduler.emplace([&]() {
            return execute_query(
            *query_ctx.query,
            *ctx->db_registry->get(BackendKind::CLOUD_NEO4J)->queryVisitor
            );
        });

        auto load_nodes_arango = scheduler.emplace([&]() {
            return execute_query(
                *query_ctx.query,
                *ctx->db_registry->get(BackendKind::ARANGO)->queryVisitor
            );
        });


        auto join_results = scheduler.then(
            [](auto... args) {
                // args are unique_ptrs moved from the tasks
                std::cout << "Got results from databases - combining started" << std::endl;
                return combine_all(std::move(args)...);
            },
            load_nodes_neo_local,
            load_nodes_neo_cloud,
            load_nodes_arango
        );

        auto gpu_data_converter = scheduler.then(
            data_to_gpu<vertex_t, edge_t, weight_t, result_t, STORE_TRANSPOSED, MULTI_GPU>,
            join_results
        );

        /*
        auto gpu_pageranks_task = scheduler.then(
            gpu_pageranks<vertex_t, edge_t, weight_t, result_t, STORE_TRANSPOSED, MULTI_GPU>,
            gpu_data_converter
        );
        */


        auto gpu_bfs_task = scheduler.then(
            gpu_bfs<vertex_t, edge_t, weight_t, result_t, STORE_TRANSPOSED, MULTI_GPU>,
            gpu_data_converter
        );


        /*
        // Invalid input arguments: triangle_count currently supports undirected graphs only.
        auto gpu_triangle_count_task = scheduler.then(
            gpu_triangle_count<vertex_t, edge_t, weight_t, result_t, STORE_TRANSPOSED, MULTI_GPU>,
            gpu_data_converter
        );
        */

        auto gpu_pageranks_data_copyback_task = scheduler.then(
            pagerank_result_to_cpu<res_t, vertex_t, edge_t, weight_t, result_t, STORE_TRANSPOSED, MULTI_GPU>,
            gpu_bfs_task // gpu_pageranks_task
        );


        scheduler.run();


        auto & h_pr = gpu_pageranks_data_copyback_task.get();

        // ----------------------------
        // Print
        // ----------------------------

        /*
        for(size_t i=0;i<h_pr.size();i++) {
            std::cout << "vertex "
                      << i
                      << " component = "
                      << h_pr[i]
                      << std::endl;
        }
        */

        return std::make_unique<QueryResult>();
    }
};



/*
auto join_results_ = scheduler.emplace([&]() {
   return combine_all(
        std::move(load_nodes_arango.get()),
        std::move(load_nodes_neo_cloud.get()),
        std::move(load_nodes_neo_local.get())
   );
});

scheduler.precede(load_nodes_arango, join_results_);
scheduler.precede(load_nodes_neo_cloud, join_results_);
scheduler.precede(load_nodes_neo_local, join_results_);
*/

#endif // ENGINE_HPP