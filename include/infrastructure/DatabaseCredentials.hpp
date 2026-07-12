/**
 * @file include/infrastructure/DatabaseCredentials.hpp
 * @brief Declarations for database credentials in the backend infrastructure module.
 */

//
// Created by akos on 7. 5. 2026.
//

#ifndef CPGDB_DATABASECREDENTIALS_HPP
#define CPGDB_DATABASECREDENTIALS_HPP

#include <string>

namespace cpgdb {
struct DatabaseCredentials {
    std::string username;
    std::string password;
};

} // namespace cpgdb

#endif // CPGDB_DATABASECREDENTIALS_HPP
