//
// Created by akos on 12. 7. 2026.
//

#ifndef CPGDB_IEVENTSERIALIZER_HPP
#define CPGDB_IEVENTSERIALIZER_HPP
#include "logging/EventLevel.hpp"
#include "utilities/json.hpp"

namespace cpgdb {

class IEventSerializer {
  public:
    virtual ~IEventSerializer() = default;
    virtual std::string serialize(EventLevel level,
                                  std::string_view event_name,
                                  json fields = nlohmann::json::object()) const = 0;
};

} // namespace cpgdb

#endif // CPGDB_IEVENTSERIALIZER_HPP
