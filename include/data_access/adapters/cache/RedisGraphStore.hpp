#pragma once

#include "data_access/IGraphStore.hpp"

#include <memory>
#include <chrono>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace polygraph {

class RedisGraphStore final : public IGraphStore {
  public:
    RedisGraphStore(BackendDescriptor descriptor, std::string endpoint, int database, std::string key_prefix,
                    std::string expected_cache_plan = {});
    ~RedisGraphStore() override;

    [[nodiscard]] BackendKind kind() const override { return BackendKind::RedisCache; }
    [[nodiscard]] const BackendDescriptor& descriptor() const override { return descriptor_; }
    [[nodiscard]] bool can_expand(const NodeRef& source, const ExpandSpec& spec) const override;

    std::vector<AdjacencyRow> get_neighbors_batch(
        std::span<const NodeRef> frontier, const ExpandSpec& spec, int depth) override;
    std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch(
        std::span<const NodeRef> frontier, const ExpandSpec& spec, int depth, const BfsOptions& options) override;
    void stream_neighbor_candidates(
        std::span<const NodeRef> frontier, const ExpandSpec& spec, int depth, const BfsOptions& options,
        const NeighborBatchConsumer& consumer) override;

  private:
    [[nodiscard]] bool cache_ready() const;
    class Client;
    BackendDescriptor descriptor_;
    std::string key_prefix_;
    std::string expected_cache_plan_;
    std::unique_ptr<Client> client_;
    mutable std::mutex readiness_mutex_;
    mutable std::optional<bool> readiness_;
    mutable std::chrono::steady_clock::time_point readiness_checked_at_{};
};

} // namespace polygraph
