#pragma once

#include "data_access/IGraphStore.hpp"
#include "infrastructure/Placement.hpp"
#include "utilities/uuid.hpp"

#include <charconv>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace polygraph::test {

    /**
     * Compatibility for old fixture literals: map a zero-prefixed UUID's final
     * twelve hexadecimal digits to a numeric ID. This does not encode an owner;
     * traversal fixtures must use BackendEncodedId with an explicit routing layout.
     * Decimal and 0x-prefixed numeric strings use the normal GlobalId parser.
     */
    inline GlobalId uuid(const char *value) {
        const std::string_view text{value};
        constexpr std::string_view legacy_prefix{"00000000-0000-0000-0000-"};
        if (!text.starts_with(legacy_prefix)) {
            return global_id_from_string(text);
        }

        const auto suffix = text.substr(legacy_prefix.size());
        GlobalId id{};
        const auto [end, error] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), id, 16);
        if (suffix.size() != 12 || error != std::errc{} || end != suffix.data() + suffix.size()) {
            throw std::invalid_argument("Invalid legacy test fixture ID");
        }
        return id;
    }

    class TestGraphStore final : public IGraphStore {
    public:
        using Graph = std::unordered_map<GlobalId, std::vector<NodeRef>>;

        TestGraphStore(BackendDescriptor descriptor, Graph graph = {}) :
            descriptor_(std::move(descriptor)), graph_(std::move(graph)) {}

        [[nodiscard]] BackendKind kind() const override { return descriptor_.kind; }

        [[nodiscard]] const BackendDescriptor &descriptor() const override { return descriptor_; }

        std::vector<AdjacencyRow> get_neighbors_batch(std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                                      int depth) override {
            ++neighbor_call_count_;
            observed_depths_.push_back(depth);

            std::vector<AdjacencyRow> rows;
            rows.reserve(frontier.size());
            for (const NodeRef &source: frontier) {
                AdjacencyRow row{.source = source, .neighbors = {}};
                if (const auto iterator = graph_.find(source.global_id); iterator != graph_.end()) {
                    row.neighbors.reserve(iterator->second.size());
                    for (const NodeRef &neighbor: iterator->second) {
                        if (spec.allows_target_type(neighbor.type)) {
                            row.neighbors.push_back(NeighborRef{.vertex = neighbor, .via_edge = std::nullopt});
                        }
                    }
                }
                rows.push_back(std::move(row));
            }
            return rows;
        }

        [[nodiscard]] bool can_pull_expand(const ExpandSpec &) const override {
            return pull_enabled_ && descriptor_.coverage.fresh && descriptor_.coverage.complete_for_requested_subgraph;
        }

        [[nodiscard]] std::size_t pull_vertex_count() const override {
            if (!pull_enabled_) {
                return 0;
            }
            std::unordered_set<GlobalId> ids;
            for (const auto &[source, neighbors]: graph_) {
                ids.insert(source);
                for (const auto &neighbor: neighbors) {
                    ids.insert(neighbor.global_id);
                }
            }
            return ids.size();
        }

        void stream_pull_candidates(const ExpandSpec &, int depth, const BfsOptions &options,
                                    const VertexMembershipPredicate &frontier_contains,
                                    const VertexMembershipPredicate &visited_contains,
                                    const PullCandidateConsumer &consumer) override {
            ++pull_call_count_;
            observed_pull_depths_.push_back(depth);
            std::unordered_map<GlobalId, NodeRef> vertices;
            for (const auto &[source, neighbors]: graph_) {
                vertices.try_emplace(source, NodeRef{.global_id = source, .type = node_type_});
                for (const auto &neighbor: neighbors) {
                    vertices.try_emplace(neighbor.global_id, neighbor);
                }
            }

            std::vector<PullCandidate> batch;
            batch.reserve(options.streaming_candidate_batch_size);
            for (const auto &[target_id, target]: vertices) {
                if (visited_contains(target_id)) {
                    continue;
                }
                const auto adjacency = graph_.find(target_id);
                if (adjacency == graph_.end()) {
                    continue;
                }
                for (const NodeRef &neighbor: adjacency->second) {
                    if (!frontier_contains(neighbor.global_id)) {
                        continue;
                    }
                    batch.push_back(PullCandidate{.vertex = target, .predecessor = neighbor});
                    if (batch.size() >= options.streaming_candidate_batch_size) {
                        consumer(std::span<const PullCandidate>(batch));
                        batch.clear();
                    }
                    break;
                }
            }
            if (!batch.empty()) {
                consumer(std::span<const PullCandidate>(batch));
            }
        }

        BfsResult run_bfs(const NodeRef &, const ExpandSpec &, const BfsOptions &) override {
            ++native_bfs_call_count_;
            return native_result_;
        }

        void enable_native_bfs(BfsResult result = {}) {
            descriptor_.capabilities.supports_native_bfs = true;
            native_result_ = std::move(result);
        }

        void enable_pull(TypeId node_type = 1) {
            pull_enabled_ = true;
            node_type_ = node_type;
        }

        [[nodiscard]] std::size_t neighbor_call_count() const noexcept { return neighbor_call_count_; }
        [[nodiscard]] std::size_t pull_call_count() const noexcept { return pull_call_count_; }

        [[nodiscard]] std::size_t native_bfs_call_count() const noexcept { return native_bfs_call_count_; }

        [[nodiscard]] const std::vector<int> &observed_depths() const noexcept { return observed_depths_; }

    private:
        BackendDescriptor descriptor_;
        Graph graph_;
        bool pull_enabled_ = false;
        TypeId node_type_ = 1;
        BfsResult native_result_;
        std::size_t neighbor_call_count_ = 0;
        std::size_t native_bfs_call_count_ = 0;
        std::size_t pull_call_count_ = 0;
        std::vector<int> observed_depths_;
        std::vector<int> observed_pull_depths_;
    };

    inline BackendDescriptor descriptor(BackendId id, int priority = 100, BackendKind kind = BackendKind::RamCache,
                                        bool fresh = true) {
        return BackendDescriptor{
                .id = id,
                .kind = kind,
                .capabilities = {.supports_batch_neighbors = true},
                .coverage =
                        {
                                .fresh = fresh,
                                .covers_all_node_types = true,
                                .covers_all_edge_types = true,
                                .complete_for_requested_subgraph = true,
                                .node_types = {},
                                .edge_types = {},
                                .graph_version = 1,
                        },
                .priority = priority,
                .name = "test-store",
        };
    }

    inline Placement placement(BackendId backend_id, TypeId node_type, PlacementKind kind = PlacementKind::Primary,
                               int priority = 100, bool authoritative = true) {
        return Placement{
                .backend_id = backend_id,
                .kind = kind,
                .source_node_types = {node_type},
                .edge_types = {},
                .target_node_types = {node_type},
                .authoritative = authoritative,
                .priority = priority,
        };
    }

} // namespace polygraph::test
