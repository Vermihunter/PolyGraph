//
// Created by akos on 12. 7. 2026.
//

#ifndef CPGDB_JSONEVENTSERIALIZER_HPP
#define CPGDB_JSONEVENTSERIALIZER_HPP
#include "IEventSerializer.hpp"

namespace cpgdb {

class JsonEventSerializer final : public IEventSerializer {
  public:
    std::string
    serialize(EventLevel level, std::string_view event_name, json fields) const override;
};

} // namespace cpgdb

#endif // CPGDB_JSONEVENTSERIALIZER_HPP
