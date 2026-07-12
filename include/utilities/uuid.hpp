/**
 * @file include/utilities/uuid.hpp
 * @brief Declarations for uuid in the utility module.
 */

//
// Created by akos on 19. 3. 2026.
//

#ifndef CPGDB_UUID_HPP
#define CPGDB_UUID_HPP

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_serialize.hpp>

namespace cpgdb {
using uuid = boost::uuids::uuid;

} // namespace cpgdb

#endif // CPGDB_UUID_HPP
