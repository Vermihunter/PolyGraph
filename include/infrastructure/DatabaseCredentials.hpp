/**
 * @file include/infrastructure/DatabaseCredentials.hpp
 * @brief Declarations for database credentials in the backend infrastructure module.
 */

//
// Created by akos on 7. 5. 2026.
//

#ifndef POLYGRAPH_DATABASECREDENTIALS_HPP
#define POLYGRAPH_DATABASECREDENTIALS_HPP

#include <string>

namespace polygraph {
struct DatabaseCredentials {
    std::string username;
    std::string password;
};

} // namespace polygraph

#endif // POLYGRAPH_DATABASECREDENTIALS_HPP
