//
// Created by akos on 11. 7. 2026.
//

#include "serialization/NodeRefJsonSerializer.hpp"

#include <boost/uuid/uuid_io.hpp>

namespace cpgdb {

    void to_json(json &value, const NodeRef &node) { value = boost::uuids::to_string(node.global_id); }
} // namespace cpgdb
