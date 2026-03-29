#ifndef RECORD_HPP
#define RECORD_HPP

#include <memory>
#include <vector>

#include <data_model/Value.hpp>

struct Record {
  //std::shared_ptr<const std::vector<std::string>> keys;
  std::vector<Value> fields;

  Record() = default;
  Record(const Record&) = delete;
  Record& operator=(const Record&) = delete;
  Record(Record&&) = default;
  Record& operator=(Record&&) = default;

  const Value& operator[](size_t i) const { return fields[i]; }
};

using record_t = std::unique_ptr<Record>;

#endif // RECORD_HPP