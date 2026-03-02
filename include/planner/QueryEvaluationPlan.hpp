#ifndef QUERY_EVALUATION_PLAN_HPP
#define QUERY_EVALUATION_PLAN_HPP

#include <vector>

#include <query/Query.hpp>

// Very not final
struct QueryEvaluationPlan {
    std::vector<Query> subqueries;
};


#endif // QUERY_EVALUATION_PLAN_HPP