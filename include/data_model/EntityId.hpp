#ifndef ENTITY_ID_HPP
#define ENTITY_ID_HPP

#include <string>
#include <boost/uuid/uuid.hpp>

using EntityId = std::string;
using GlobalId = boost::uuids::uuid;

#endif // ENTITY_ID_HPP