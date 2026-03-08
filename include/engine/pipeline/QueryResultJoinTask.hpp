//
// Created by akos on 8. 3. 2026.
//

#ifndef CPGDB_QUERYRESULTJOINTASK_HPP
#define CPGDB_QUERYRESULTJOINTASK_HPP

#include <query/Query.hpp>


template<typename T, typename... Rest>
T combine_all(T first, Rest... rest) {

    std::cout << "combining" << std::endl;
    (first->combine(std::move(rest)), ...);
    return first;
}


#endif //CPGDB_QUERYRESULTJOINTASK_HPP