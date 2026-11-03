#include "support/ScriptedGraphStore.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

namespace polygraph::test {

    void ScriptedGate::arrive_and_wait() {
        std::unique_lock lock(mutex_);
        ++arrivals_;
        condition_.notify_all();
        if (!condition_.wait_for(lock, std::chrono::seconds(5), [this] { return released_; }))
            throw std::runtime_error("ScriptedGate release timed out");
    }
    bool ScriptedGate::wait_for_arrivals(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] { return arrivals_ >= count; });
    }
    void ScriptedGate::release() {
        {
            std::scoped_lock lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }
    std::size_t ScriptedGate::arrivals() const {
        std::scoped_lock lock(mutex_);
        return arrivals_;
    }

    class ScriptedGraphStore::ActiveCall {
    public:
        ActiveCall(ScriptedGraphStore &store, ExpansionCall call) : store_(store), call_(std::move(call)) {
            std::scoped_lock lock(store_.state_mutex_);
            store_.calls_.push_back(call_);
            ++store_.active_calls_;
        }
        ~ActiveCall() {
            std::scoped_lock lock(store_.state_mutex_);
            --store_.active_calls_;
            ++store_.completed_calls_;
        }
        void start() const {
            if (store_.config_.before_expand)
                store_.config_.before_expand(call_);
            if (store_.config_.fail_before_depth == call_.depth)
                throw std::runtime_error("scripted expansion failure");
        }
        void finish() const {
            if (store_.config_.after_expand)
                store_.config_.after_expand(call_);
        }

    private:
        ScriptedGraphStore &store_;
        const ExpansionCall call_;
    };

    ScriptedGraphStore::ScriptedGraphStore(BackendDescriptor descriptor,
                                           const std::shared_ptr<const ScriptedTopology> &topology,
                                           std::vector<GlobalId> resident_sources, ScriptedStoreConfig config) :
        descriptor_(std::move(descriptor)),
        topology_(topology ? std::make_shared<const ScriptedTopology>(*topology) : nullptr),
        residents_(resident_sources.begin(), resident_sources.end()), config_(std::move(config)) {
        if (!topology_ || config_.candidate_workers == 0)
            throw std::invalid_argument("invalid scripted store config");
        for (const auto &vertex: topology_->vertices) {
            if (!vertices_.emplace(vertex.global_id, vertex).second)
                throw std::invalid_argument("duplicate scripted vertex ID");
        }
        for (const auto &edge: topology_->edges) {
            if (!vertices_.contains(edge.source.global_id) || !vertices_.contains(edge.target.global_id))
                throw std::invalid_argument("scripted edge endpoint absent");
        }
        for (const auto id: residents_)
            if (!vertices_.contains(id))
                throw std::invalid_argument("scripted resident absent");
    }
    BackendKind ScriptedGraphStore::kind() const { return descriptor_.kind; }
    const BackendDescriptor &ScriptedGraphStore::descriptor() const { return descriptor_; }
    bool ScriptedGraphStore::can_expand(const NodeRef &source, const ExpandSpec &) const {
        return descriptor_.coverage.fresh && descriptor_.coverage.complete_for_requested_subgraph &&
               residents_.contains(source.global_id);
    }
    bool ScriptedGraphStore::can_run_native_bfs(const NodeRef &, const ExpandSpec &, const BfsOptions &) const {
        return false;
    }
    bool ScriptedGraphStore::can_pull_expand(const ExpandSpec &spec) const {
        return config_.complete_pull && !config_.partial_pull && spec.direction == Direction::Both &&
               descriptor_.coverage.fresh && descriptor_.coverage.complete_for_requested_subgraph &&
               residents_.size() == vertices_.size();
    }
    std::size_t ScriptedGraphStore::pull_vertex_count() const { return vertices_.size(); }
    NodeRef ScriptedGraphStore::resolve(NodeRef source) const { return vertices_.at(source.global_id); }
    ExpansionCall ScriptedGraphStore::make_call(std::span<const NodeRef> frontier, int depth, ExpansionKind kind,
                                                std::uint64_t query_id) const {
        return {descriptor_.id, depth, kind, {frontier.begin(), frontier.end()}, query_id};
    }
    std::vector<MinimalAdjacencyRow> ScriptedGraphStore::expand_rows(std::span<const NodeRef> frontier,
                                                                     const ExpandSpec &spec) const {
        std::vector<MinimalAdjacencyRow> rows;
        for (auto source: frontier) {
            if (!can_expand(source, spec))
                throw std::runtime_error("nonresident scripted source");
            MinimalAdjacencyRow row{resolve(source), {}};
            for (const auto &edge: topology_->edges) {
                if (!spec.allows_edge_type(edge.type))
                    continue;
                const auto add = [&](const NodeRef &target) {
                    const auto vertex = resolve(target);
                    if (spec.allows_target_type(vertex.type))
                        row.neighbors.push_back(vertex);
                };
                if (edge.source.global_id == source.global_id)
                    add(edge.target);
                if (spec.direction == Direction::Both && edge.target.global_id == source.global_id)
                    add(edge.source);
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }
    std::vector<MinimalAdjacencyRow> ScriptedGraphStore::get_minimal_neighbors_batch(std::span<const NodeRef> frontier,
                                                                                     const ExpandSpec &spec, int depth,
                                                                                     const BfsOptions &options) {
        ActiveCall call(*this, make_call(frontier, depth, ExpansionKind::Rows, options.scheduler_query_id));
        call.start();
        auto rows = expand_rows(frontier, spec);
        call.finish();
        return rows;
    }
    std::vector<AdjacencyRow> ScriptedGraphStore::get_neighbors_batch(std::span<const NodeRef> frontier,
                                                                      const ExpandSpec &spec, int depth) {
        auto minimal = get_minimal_neighbors_batch(frontier, spec, depth, {});
        std::vector<AdjacencyRow> rows;
        for (auto &row: minimal) {
            AdjacencyRow rich{row.source, {}};
            for (const auto &neighbor: row.neighbors)
                rich.neighbors.push_back({neighbor, std::nullopt});
            rows.push_back(std::move(rich));
        }
        return rows;
    }
    void ScriptedGraphStore::stream_neighbor_candidates(std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                                        int depth, const BfsOptions &options,
                                                        const NeighborBatchConsumer &consumer) {
        const auto metadata = make_call(frontier, depth, ExpansionKind::Candidates, options.scheduler_query_id);
        ActiveCall call(*this, metadata);
        call.start();
        if (!consumer || options.streaming_candidate_batch_size == 0)
            throw std::invalid_argument("invalid scripted candidate consumer/batch size");
        if (config_.fail_after_batches == 0) {
            throw std::runtime_error("scripted candidate failure");
        }
        std::vector<NodeRef> candidates;
        for (const auto &row: expand_rows(frontier, spec))
            candidates.insert(candidates.end(), row.neighbors.begin(), row.neighbors.end());
        const auto batch_size = options.streaming_candidate_batch_size;
        const auto batch_count = candidates.size() / batch_size + (candidates.size() % batch_size != 0);
        std::atomic_size_t next{0};
        std::atomic_size_t delivered{0};
        std::mutex error_mutex;
        std::exception_ptr error;
        const auto work = [&] {
            try {
                // One reusable buffer per worker: callbacks must consume/copy before returning.
                std::vector<NodeRef> buffer;
                while (true) {
                    const auto ordinal = next.fetch_add(1);
                    if (ordinal >= batch_count)
                        break;
                    const auto begin = ordinal * batch_size;
                    buffer.assign(candidates.begin() + static_cast<std::ptrdiff_t>(begin),
                                  candidates.begin() +
                                          static_cast<std::ptrdiff_t>(std::min(begin + batch_size, candidates.size())));
                    if (config_.before_candidate_batch)
                        config_.before_candidate_batch(metadata, ordinal);
                    consumer(buffer);
                    if (config_.poison_candidate_buffers)
                        std::fill(buffer.begin(), buffer.end(), NodeRef{0, unresolved_type_id});
                    if (config_.after_candidate_batch)
                        config_.after_candidate_batch(metadata, ordinal);
                    if (config_.fail_after_batches && delivered.fetch_add(1) + 1 >= *config_.fail_after_batches)
                        throw std::runtime_error("scripted candidate failure");
                }
            } catch (...) {
                std::scoped_lock lock(error_mutex);
                if (!error)
                    error = std::current_exception();
            }
        };
        if (config_.candidate_workers == 1)
            work();
        else {
            std::vector<std::jthread> workers;
            workers.reserve(std::min(config_.candidate_workers, batch_count));
            for (std::size_t i = 0; i < std::min(config_.candidate_workers, batch_count); ++i)
                workers.emplace_back(work);
        } // All workers joined before rethrow, hooks, and destruction of captured state.
        if (error)
            std::rethrow_exception(error);
        call.finish();
    }
    void ScriptedGraphStore::stream_pull_candidates(const ExpandSpec &spec, int depth, const BfsOptions &options,
                                                    const VertexMembershipPredicate &frontier_contains,
                                                    const VertexMembershipPredicate &visited_contains,
                                                    const PullCandidateConsumer &consumer) {
        ActiveCall call(*this, make_call({}, depth, ExpansionKind::Pull, options.scheduler_query_id));
        call.start();
        if (!can_pull_expand(spec) || !consumer || options.streaming_candidate_batch_size == 0)
            throw std::invalid_argument("invalid scripted pull");
        std::vector<PullCandidate> buffer;
        const auto flush = [&] {
            consumer(buffer);
            if (config_.poison_candidate_buffers)
                std::fill(buffer.begin(), buffer.end(),
                          PullCandidate{{0, unresolved_type_id}, {0, unresolved_type_id}});
            buffer.clear();
        };
        for (const auto &target: topology_->vertices) {
            if (visited_contains(target.global_id) || !spec.allows_target_type(target.type))
                continue;
            for (const auto &edge: topology_->edges) {
                if (!spec.allows_edge_type(edge.type))
                    continue;
                std::optional<NodeRef> parent;
                if (edge.target.global_id == target.global_id && frontier_contains(edge.source.global_id))
                    parent = resolve(edge.source);
                else if (edge.source.global_id == target.global_id && frontier_contains(edge.target.global_id))
                    parent = resolve(edge.target);
                if (!parent)
                    continue;
                buffer.push_back({target, *parent});
                if (buffer.size() == options.streaming_candidate_batch_size)
                    flush();
                break;
            }
        }
        if (!buffer.empty())
            flush();
        call.finish();
    }
    std::vector<ExpansionCall> ScriptedGraphStore::calls() const {
        std::scoped_lock lock(state_mutex_);
        return calls_;
    }
    std::size_t ScriptedGraphStore::active_calls() const {
        std::scoped_lock lock(state_mutex_);
        return active_calls_;
    }
    std::size_t ScriptedGraphStore::completed_calls() const {
        std::scoped_lock lock(state_mutex_);
        return completed_calls_;
    }
} // namespace polygraph::test
