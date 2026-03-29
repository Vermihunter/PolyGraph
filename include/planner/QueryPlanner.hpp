#ifndef QUERY_PLANNER_HPP
#define QUERY_PLANNER_HPP

#include <memory>

#include <query/QueryContext.hpp>
#include <planner/QueryEvaluationPlan.hpp>

/**
 * Decides how to execute a given query including - creates a graph of execution instance:
 * - dividing the queries into subqueries (how to interact with DBs)
 * - 
 */

class QueryPlanner {

public:
    QueryEvaluationPlan get_plan_for(QueryContext&& query) {
        QueryEvaluationPlan plan;

        if (query.targetDB != DatabaseTypes::MIXED) {
            plan.subqueries.push_back(std::move(query));
            return plan;
        }

        plan.subqueries.push_back(QueryContext {
            .query = query.query->clone(),
            .targetDB = DatabaseTypes::ARANGO,
        });

        plan.subqueries.push_back(QueryContext {
            .query = query.query->clone(),
            .targetDB = DatabaseTypes::NEO4J,
        });

        return plan;
    }
};

using query_planner_t = std::unique_ptr<QueryPlanner>;


#endif // QUERY_PLANNER_HPP