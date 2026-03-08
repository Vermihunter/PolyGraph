//
// Created by akos on 8. 3. 2026.
//

#ifndef CPGDB_QUERYTASK_HPP
#define CPGDB_QUERYTASK_HPP

#include <chrono>

#include <query/Query.hpp>

inline query_result_t execute_query(Query& query, QueryVisitor &visitor) {
    return query.accept(visitor);
}

#endif //CPGDB_QUERYTASK_HPP