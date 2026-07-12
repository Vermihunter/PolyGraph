//
// Created by akos on 12. 7. 2026.
//

#include "logging/sinks/JsonEventSerializer.hpp"

namespace cpgdb {

    std::string JsonEventSerializer::serialize(EventLevel _level, std::string_view _name, json fields) const {
        return fields.dump();
    }

} // namespace cpgdb
