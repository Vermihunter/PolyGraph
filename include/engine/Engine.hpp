#ifndef ENGINE_HPP
#define ENGINE_HPP

#include <query/QueryResult.hpp>
#include <query/Query.hpp>
#include <query/QueryContext.hpp>
#include <engine/DatabaseContext.hpp>
#include <raft/core/handle.hpp>

#include "GPU/DataModelToGPUGraphConverter.hpp"
#include "pipeline/QueryExecutionTask.hpp"
#include "pipeline/QueryResultJoinTask.hpp"
#include "pipeline/Scheduler.hpp"
#include "query/query_types/GetAllVerticesQuery.hpp"

#include "data_access/adapters/cuGraph/cuGraphExample.hpp"

class Engine {
    db_context_t ctx;
public:
    Engine(db_context_t _ctx) : ctx(std::move(_ctx)) {}

    query_result_t evaluate(QueryContext&& query_ctx) {
        std::cout << "Evaluating in engine" << std::endl;

        /*
        auto plan = ctx->planner->get_plan_for(std::move(query_ctx));

        auto res = std::make_unique<QueryResult>();
        for (auto&& subquery : plan.subqueries) {
            auto& db_ctx = ctx->db_registry->get(subquery.targetDB);
            res->combine(subquery.query->accept(*db_ctx->queryVisitor));
        }
        */


        Scheduler scheduler;

        auto load_nodes_neo_local = scheduler.emplace([&]() {
            return execute_query(
                *query_ctx.query,
                *ctx->db_registry->get(DatabaseTypes::NEO4J)->queryVisitor
            );
        });

        auto load_nodes_neo_cloud = scheduler.emplace([&]() {
            return execute_query(
            *query_ctx.query,
            *ctx->db_registry->get(DatabaseTypes::CLOUD_NEO4J)->queryVisitor
            );
        });

        auto load_nodes_arango = scheduler.emplace([&]() {
            return execute_query(
                *query_ctx.query,
                *ctx->db_registry->get(DatabaseTypes::ARANGO)->queryVisitor
            );
        });

        auto join_results = scheduler.then(
            [](auto... args) {
                // args are unique_ptrs moved from the tasks
                return combine_all(std::move(args)...);
            },
            load_nodes_neo_local,
            load_nodes_neo_cloud,
            load_nodes_arango
        );


        scheduler.run();


        raft::handle_t handle;
        DataModelToGPUGraphConverter converter;
        auto results = std::move(join_results).get();

        auto gpudata =
            converter.convert<int32_t, int32_t, float, float>(handle, results->data);


        using result_t = float;
        auto [pageranks, metadata] =
            cugraph::pagerank<
                int32_t,
                int32_t,
                float,
                result_t,
                false
            >(
                gpudata->handle_,
                gpudata->graph_view_,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                result_t{0.85},
                result_t{1e-6},
                100,
                true
            );

        std::vector<result_t> h_pr(gpudata->id_to_string_.size());

        auto stream = gpudata->handle_.get_stream();
        cuda_check(cudaMemcpyAsync(
            h_pr.data(),
            pageranks.data(),
            h_pr.size()*sizeof(result_t),
            cudaMemcpyDeviceToHost,
            stream), "copy pagerank");

        cuda_check(cudaStreamSynchronize(stream), "sync D2H");

        // ----------------------------
        // Print
        // ----------------------------

        for(size_t i=0;i<h_pr.size();i++)
        {
            std::cout << "vertex "
                      << i
                      << " pagerank = "
                      << h_pr[i]
                      << std::endl;
        }

        return results;
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