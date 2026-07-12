/**
 * @file include/data_model/EntityId.hpp
 * @brief Declarations for entity id in the graph data-model module.
 */

#ifndef ENTITY_ID_HPP
#define ENTITY_ID_HPP

#include <boost/uuid/uuid.hpp>

namespace cpgdb {
using GlobalId = boost::uuids::uuid;

} // namespace cpgdb

#endif // ENTITY_ID_HPP
