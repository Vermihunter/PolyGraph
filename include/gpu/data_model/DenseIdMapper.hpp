/**
 * @file include/gpu/data_model/DenseIdMapper.hpp
 * @brief Compact dense-to-global ID mapping for GPU projections.
 */

#ifndef POLYGRAPH_DENSEIDMAPPER_HPP
#define POLYGRAPH_DENSEIDMAPPER_HPP

#include "utilities/uuid.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

namespace polygraph {

/**
 * Maps sorted external uint64 GlobalId values to dense cuGraph IDs.
 *
 * Only one UUID vector is retained. Reverse lookup uses binary search instead
 * of a second unordered_map, trading a small amount of lookup CPU for a much
 * smaller and predictable host-memory footprint.
 */
template <typename vertex_t> class DenseIdMapper {
  public:
    explicit DenseIdMapper(std::vector<GlobalId> ids) : to_global_(std::move(ids)) {
        std::ranges::sort(to_global_);
        to_global_.erase(std::unique(to_global_.begin(), to_global_.end()), to_global_.end());
        to_global_.shrink_to_fit();
        validate_size();
    }

    /**
     * Build from an artifact that was already sorted and deduplicated offline.
     * This is intentionally a separate API so normal callers cannot accidentally
     * skip the expensive-but-required normalization step.
     */
    [[nodiscard]] static DenseIdMapper from_sorted_unique(std::vector<GlobalId> ids) {
        if (!std::ranges::is_sorted(ids) || std::ranges::adjacent_find(ids) != ids.end()) {
            throw std::invalid_argument("Prebuilt GPU GlobalId map must be sorted and unique");
        }
        DenseIdMapper mapper(std::move(ids), SortedUniqueTag{});
        mapper.validate_size();
        return mapper;
    }

    [[nodiscard]] bool contains(const GlobalId &id) const noexcept {
        return std::ranges::binary_search(to_global_, id);
    }

    [[nodiscard]] vertex_t at(const GlobalId &id) const {
        const auto iterator = std::ranges::lower_bound(to_global_, id);
        if (iterator == to_global_.end() || *iterator != id) {
            throw std::runtime_error("GlobalId not present in DenseIdMapper: " + global_id_to_string(id));
        }
        return static_cast<vertex_t>(std::distance(to_global_.begin(), iterator));
    }

    [[nodiscard]] const GlobalId &global_id(const vertex_t dense) const {
        return to_global_.at(static_cast<std::size_t>(dense));
    }

    [[nodiscard]] std::size_t size() const noexcept { return to_global_.size(); }
    [[nodiscard]] std::size_t required_bytes() const noexcept { return to_global_.size() * sizeof(GlobalId); }
    [[nodiscard]] std::size_t allocated_bytes() const noexcept { return to_global_.capacity() * sizeof(GlobalId); }
    [[nodiscard]] const std::vector<GlobalId> &dense_to_global() const noexcept { return to_global_; }

  private:
    struct SortedUniqueTag {};

    DenseIdMapper(std::vector<GlobalId> ids, SortedUniqueTag) : to_global_(std::move(ids)) {}

    void validate_size() const {
        if (to_global_.size() > static_cast<std::size_t>(std::numeric_limits<vertex_t>::max())) {
            throw std::overflow_error("GPU projection contains more vertices than its dense ID type can represent");
        }
    }

    std::vector<GlobalId> to_global_;
};

} // namespace polygraph

#endif // POLYGRAPH_DENSEIDMAPPER_HPP
