#pragma once

#include "data_access/IGraphStore.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace polygraph::test {

    struct ScriptedEdge {
        NodeRef source;
        NodeRef target;
        TypeId type;
    };

    struct ScriptedTopology {
        std::vector<NodeRef> vertices;
        std::vector<ScriptedEdge> edges;
    };

    enum class ExpansionKind { Rows, Candidates, Pull };

    struct ExpansionCall {
        BackendId backend;
        int depth;
        ExpansionKind kind;
        std::vector<NodeRef> frontier;
        std::uint64_t query_id;
    };

    struct ScriptedStoreConfig {
        bool complete_pull = false;
        bool partial_pull = false;
        std::size_t candidate_workers = 1;
        bool poison_candidate_buffers = true;
        std::function<void(const ExpansionCall &)> before_expand;
        std::function<void(const ExpansionCall &)> after_expand;
        // Batch ordinals start at zero. Hooks may be invoked concurrently.
        std::function<void(const ExpansionCall &, std::size_t)> before_candidate_batch;
        std::function<void(const ExpansionCall &, std::size_t)> after_candidate_batch;
        std::optional<int> fail_before_depth;
        // Throw after this many callbacks have returned; zero fails before delivery.
        std::optional<std::size_t> fail_after_batches;
    };

    // Bounded test synchronization; all state is guarded by mutex_.
    class ScriptedGate {
    public:
        void arrive_and_wait();
        [[nodiscard]] bool wait_for_arrivals(std::size_t count,
                                             std::chrono::milliseconds timeout = std::chrono::seconds(5));
        void release();
        [[nodiscard]] std::size_t arrivals() const;

    private:
        mutable std::mutex mutex_;
        std::condition_variable condition_;
        std::size_t arrivals_ = 0;
        bool released_ = false;
    };

    // Immutable one-hop fake. External callbacks always run outside state_mutex_.
    class ScriptedGraphStore final : public IGraphStore {
    public:
        ScriptedGraphStore(BackendDescriptor descriptor, const std::shared_ptr<const ScriptedTopology> &topology,
                           std::vector<GlobalId> resident_sources, ScriptedStoreConfig config = {});

        [[nodiscard]] BackendKind kind() const override;
        [[nodiscard]] const BackendDescriptor &descriptor() const override;
        [[nodiscard]] bool can_expand(const NodeRef &source, const ExpandSpec &spec) const override;
        [[nodiscard]] bool can_run_native_bfs(const NodeRef &, const ExpandSpec &, const BfsOptions &) const override;
        [[nodiscard]] bool can_pull_expand(const ExpandSpec &spec) const override;
        [[nodiscard]] std::size_t pull_vertex_count() const override;

        std::vector<AdjacencyRow> get_neighbors_batch(std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                                      int depth) override;
        std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch(std::span<const NodeRef> frontier,
                                                                     const ExpandSpec &spec, int depth,
                                                                     const BfsOptions &options) override;
        void stream_neighbor_candidates(std::span<const NodeRef> frontier, const ExpandSpec &spec, int depth,
                                        const BfsOptions &options, const NeighborBatchConsumer &consumer) override;
        void stream_pull_candidates(const ExpandSpec &spec, int depth, const BfsOptions &options,
                                    const VertexMembershipPredicate &frontier_contains,
                                    const VertexMembershipPredicate &visited_contains,
                                    const PullCandidateConsumer &consumer) override;

        [[nodiscard]] std::vector<ExpansionCall> calls() const;
        [[nodiscard]] std::size_t active_calls() const;
        [[nodiscard]] std::size_t completed_calls() const;

    private:
        class ActiveCall;
        [[nodiscard]] NodeRef resolve(NodeRef source) const;
        [[nodiscard]] std::vector<MinimalAdjacencyRow> expand_rows(std::span<const NodeRef> frontier,
                                                                   const ExpandSpec &spec) const;
        [[nodiscard]] ExpansionCall make_call(std::span<const NodeRef> frontier, int depth, ExpansionKind kind,
                                              std::uint64_t query_id) const;

        const BackendDescriptor descriptor_;
        const std::shared_ptr<const ScriptedTopology> topology_;
        const std::unordered_set<GlobalId> residents_;
        const ScriptedStoreConfig config_;
        std::unordered_map<GlobalId, NodeRef> vertices_; // Built during construction, immutable afterwards.

        mutable std::mutex state_mutex_;
        std::vector<ExpansionCall> calls_; // Guarded by state_mutex_.
        std::size_t active_calls_ = 0; // Guarded by state_mutex_.
        std::size_t completed_calls_ = 0; // Guarded by state_mutex_.
    };

} // namespace polygraph::test
