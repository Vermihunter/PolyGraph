//
// Created by akos on 11. 7. 2026.
//

#include "serialization/NodeRefJsonSerializer.hpp"
#include "utilities/uuid.hpp"


namespace cpgdb {

    void to_json(json &value, const NodeRef &node) { value = global_id_to_string(node.global_id); }
} // namespace cpgdb
