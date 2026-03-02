#ifndef RECORD_HPP
#define RECORD_HPP

#include <memory>
#include <vector>
#include <unordered_map>

#include <data_model/Value.hpp>

struct Record {
  std::shared_ptr<const std::vector<std::string>> keys;
  std::vector<Value> fields;
  std::unordered_map<std::string, size_t> index;

  const Value& operator[](size_t i) const { return fields[i]; }
  const Value& operator[](const std::string& key) const { return fields.at(index.at(key)); }
};

#endif // RECORD_HPP