//
// Created by akos on 12. 7. 2026.
//

#ifndef CPGDB_SPDLOGEVENTSINK_HPP
#define CPGDB_SPDLOGEVENTSINK_HPP

#include "IEventSerializer.hpp"
#include "IEventSink.hpp"

#include <memory>
#include <spdlog/logger.h>

namespace cpgdb {

class SpdlogEventSink final : public IEventSink {
  public:
    explicit SpdlogEventSink(std::shared_ptr<spdlog::logger> logger,
                             std::unique_ptr<IEventSerializer> serializer);

    void write(EventLevel level, std::string_view event_name, const json& fields) override;

    void flush() override;

  private:
    std::shared_ptr<spdlog::logger> logger_;
    std::unique_ptr<IEventSerializer> serializer_;
};

} // namespace cpgdb

#endif // CPGDB_SPDLOGEVENTSINK_HPP
