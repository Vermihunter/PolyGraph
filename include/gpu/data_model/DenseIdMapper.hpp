/**
 * @file include/gpu/data_model/DenseIdMapper.hpp
 * @brief Declarations for dense id mapper in the GPU acceleration module.
 */

#ifndef CPGDB_DENSEIDMAPPER_HPP
#define CPGDB_DENSEIDMAPPER_HPP

#include "utilities/uuid.hpp"

#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace cpgdb {
/*
 * Maps external uint64 GlobalId values to dense cuGraph IDs:
 *
 *   GlobalId <-> 0, 1, 2, ..., n - 1
 *
 * cuGraph works best with dense vertex ids. Your graph engine should continue
 * to use GlobalId everywhere outside the GPU projection.
 */
template <typename vertex_t> class DenseIdMapper {
  public:
    vertex_t get_or_add(const GlobalId& id) {
        auto it = to_dense_.find(id);

        if (it != to_dense_.end()) {
            return it->second;
        }

        vertex_t dense = static_cast<vertex_t>(to_global_.size());

        to_dense_.emplace(id, dense);
        to_global_.push_back(id);

        return dense;
    }

    bool contains(const GlobalId& id) const {
        return to_dense_.find(id) != to_dense_.end();
    }

    vertex_t at(const GlobalId& id) const {
        auto it = to_dense_.find(id);

        if (it == to_dense_.end()) {
            throw std::runtime_error("GlobalId not present in DenseIdMapper: " +
                                     global_id_to_string(id));
        }

        return it->second;
    }

    const GlobalId& global_id(vertex_t dense) const {
        return to_global_.at(static_cast<std::size_t>(dense));
    }

    std::size_t size() const {
        return to_global_.size();
    }

    const std::vector<GlobalId>& dense_to_global() const {
        return to_global_;
    }

  private:
    std::unordered_map<GlobalId, vertex_t, std::hash<GlobalId>> to_dense_;
    std::vector<GlobalId> to_global_;
};

} // namespace cpgdb

#endif // CPGDB_DENSEIDMAPPER_HPP
