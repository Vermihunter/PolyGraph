//
// Created by akos on 12. 7. 2026.
//

#ifndef CPGDB_IEVENTSINK_HPP
#define CPGDB_IEVENTSINK_HPP

#include "logging/EventLevel.hpp"
#include "utilities/json.hpp"

#include <string_view>

namespace cpgdb {

class IEventSink {
  public:
    virtual ~IEventSink() = default;

    virtual void write(EventLevel level, std::string_view event_name, const json& fields) = 0;

    virtual void flush() = 0;
};

} // namespace cpgdb

#endif // CPGDB_IEVENTSINK_HPP
