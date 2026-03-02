#ifndef QUERY_PLANNER_HPP
#define QUERY_PLANNER_HPP

#include <memory>

#include <planner/QueryEvaluationPlan.hpp>
#include <query/Query.hpp>

/**
 * Decides how to execute a given query including - creates a graph of execution instance:
 * - dividing the queries into subqueries (how to interact with DBs)
 * - 
 */
class QueryPlanner {

public:
    QueryEvaluationPlan get_plan_for(const Query& query) {
        if (query.targetDB != DatabaseTypes::MIXED) {
            return QueryEvaluationPlan { std::vector<Query>{query}};
        }

        QueryEvaluationPlan hardCodedPlan{
            std::vector<Query>{
                Query{
                    "",
                    DatabaseTypes::ARANGO
                },
                Query{
                    "",
                    DatabaseTypes::JANUS
                },
            }
        };

        return hardCodedPlan;
    }
};

using query_planner_t = std::unique_ptr<QueryPlanner>;


#endif // QUERY_PLANNER_HPP