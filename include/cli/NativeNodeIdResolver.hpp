/**
 * @file include/cli/NativeNodeIdResolver.hpp
 * @brief Sparse, lazy lookup for polygraph.native-id-map node records.
 */

#pragma once

#include "data_model/EntityId.hpp"

#include <cstddef>
#include <filesystem>
#include <ios>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace polygraph::cli {

/**
 * Resolves original generated node IDs to IDs produced by the latest backend load.
 *
 * The sidecar is sorted by original node ID. Only one entry per @p index_stride
 * node records is retained, while resolved IDs are cached on demand. This avoids
 * loading a potentially multi-gigabyte ID map into process memory merely to start
 * a BFS from a handful of IDs.
 */
class NativeNodeIdResolver {
  public:
    explicit NativeNodeIdResolver(std::filesystem::path path, std::size_t index_stride = 4096);

    [[nodiscard]] GlobalId resolve(GlobalId input_id) const;
    [[nodiscard]] std::size_t sparse_index_entries() const noexcept { return sparse_index_.size(); }
    [[nodiscard]] std::size_t sparse_index_bytes() const noexcept {
        return sparse_index_.capacity() * sizeof(IndexEntry);
    }
    [[nodiscard]] std::size_t cached_entries() const;

  private:
    struct IndexEntry {
        GlobalId old_id;
        std::streamoff offset;
    };

    void build_sparse_index(std::size_t index_stride);

    std::filesystem::path path_;
    std::vector<IndexEntry> sparse_index_;
    mutable std::unordered_map<GlobalId, GlobalId> cache_;
    mutable std::mutex mutex_;
};

} // namespace polygraph::cli
