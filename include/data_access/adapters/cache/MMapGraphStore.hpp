#pragma once

#include "data_access/IGraphStore.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace polygraph {

class MMapGraphStore final : public IGraphStore {
  public:
    MMapGraphStore(BackendDescriptor descriptor, std::filesystem::path directory);
    ~MMapGraphStore() override;

    MMapGraphStore(const MMapGraphStore&) = delete;
    MMapGraphStore& operator=(const MMapGraphStore&) = delete;

    [[nodiscard]] BackendKind kind() const override { return BackendKind::MemoryMapped; }
    [[nodiscard]] const BackendDescriptor& descriptor() const override { return descriptor_; }
    [[nodiscard]] bool can_expand(const NodeRef& source, const ExpandSpec& spec) const override;
    [[nodiscard]] bool can_pull_expand(const ExpandSpec& spec) const override;
    [[nodiscard]] std::size_t pull_vertex_count() const override { return index_count_; }
    void stream_pull_candidates(
        const ExpandSpec& spec, int depth, const BfsOptions& options,
        const VertexMembershipPredicate& frontier_contains,
        const VertexMembershipPredicate& visited_contains,
        const PullCandidateConsumer& consumer) override;

    std::vector<AdjacencyRow> get_neighbors_batch(
        std::span<const NodeRef> frontier, const ExpandSpec& spec, int depth) override;
    std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch(
        std::span<const NodeRef> frontier, const ExpandSpec& spec, int depth, const BfsOptions& options) override;

  private:
    struct IndexRecord {
        std::uint64_t canonical_id;
        std::uint64_t neighbor_offset;
        std::uint64_t neighbor_count;
    };

    [[nodiscard]] const IndexRecord* find(std::uint64_t canonical_id) const noexcept;
    void close_files() noexcept;

    BackendDescriptor descriptor_;
    std::filesystem::path directory_;
    int index_fd_ = -1;
    int neighbors_fd_ = -1;
    const IndexRecord* index_ = nullptr;
    const std::uint64_t* neighbors_ = nullptr;
    std::size_t index_count_ = 0;
    std::size_t neighbor_count_ = 0;
    std::size_t index_bytes_ = 0;
    std::size_t neighbor_bytes_ = 0;
};

} // namespace polygraph
