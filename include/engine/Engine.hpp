#ifndef ENGINE_HPP
#define ENGINE_HPP

#include <query/QueryResult.hpp>
#include <query/Query.hpp>
#include <engine/DatabaseContext.hpp>

class Engine {
    db_context_t ctx;
public:
    Engine(db_context_t _ctx) : ctx(std::move(_ctx)) {}

    QueryResult evaluate(const Query& query) {
        QueryEvaluationPlan plan = ctx->planner->get_plan_for(query);
        
        const Query& subquery = plan.subqueries[0];
        IDatabase * evaluator = ctx->db_registry->get(subquery.targetDB);
        return evaluator->evaluate(subquery);
    }
};


#endif // ENGINE_HPP