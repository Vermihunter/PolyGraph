#include "data_access/adapters/cache/MMapGraphStore.hpp"

#include "events/Events.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "observability/MetricsAccess.hpp"

#include <algorithm>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace polygraph {
    namespace {

        std::pair<int, std::size_t> open_readonly(const std::filesystem::path &path) {
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                throw std::runtime_error("Could not open mmap cache file: " + path.string());
            }
            struct stat st{};
            if (::fstat(fd, &st) != 0) {
                ::close(fd);
                throw std::runtime_error("Could not stat mmap cache file: " + path.string());
            }
            return {fd, static_cast<std::size_t>(st.st_size)};
        }

        bool supports_cache_spec(const ExpandSpec &spec) {
            return spec.direction == Direction::Both && spec.allowed_edge_types.empty() &&
                   spec.allowed_target_node_types.empty() && !spec.return_edge_refs;
        }

    } // namespace

    MMapGraphStore::MMapGraphStore(BackendDescriptor descriptor, std::filesystem::path directory) :
        descriptor_(std::move(descriptor)), directory_(std::move(directory)) {
        static_assert(sizeof(IndexRecord) == 24, "cache index format is three packed uint64 values");
        try {
            auto [index_fd, index_bytes] = open_readonly(directory_ / "index.bin");
            index_fd_ = index_fd;
            index_bytes_ = index_bytes;
            auto [neighbors_fd, neighbors_bytes] = open_readonly(directory_ / "neighbors.bin");
            neighbors_fd_ = neighbors_fd;
            neighbor_bytes_ = neighbors_bytes;
            if (index_bytes_ % sizeof(IndexRecord) != 0 || neighbor_bytes_ % sizeof(std::uint64_t) != 0) {
                throw std::runtime_error("Invalid PolyGraph mmap cache binary format in " + directory_.string());
            }
            index_count_ = index_bytes_ / sizeof(IndexRecord);
            neighbor_count_ = neighbor_bytes_ / sizeof(std::uint64_t);
            if (index_bytes_ != 0) {
                void *mapped = ::mmap(nullptr, index_bytes_, PROT_READ, MAP_SHARED, index_fd_, 0);
                if (mapped == MAP_FAILED) {
                    throw std::runtime_error("mmap(index.bin) failed for " + directory_.string());
                }
                index_ = static_cast<const IndexRecord *>(mapped);
            }
            if (neighbor_bytes_ != 0) {
                void *mapped = ::mmap(nullptr, neighbor_bytes_, PROT_READ, MAP_SHARED, neighbors_fd_, 0);
                if (mapped == MAP_FAILED) {
                    throw std::runtime_error("mmap(neighbors.bin) failed for " + directory_.string());
                }
                neighbors_ = static_cast<const std::uint64_t *>(mapped);
            }
            observability::set_mmap_cache_metrics(observability::MMapCacheMetrics{
                    .mapped_bytes = index_bytes_ + neighbor_bytes_,
                    .index_bytes = index_bytes_,
                    .neighbor_bytes = neighbor_bytes_,
                    .vertices = index_count_,
                    .relations = neighbor_count_,
            });
        } catch (...) {
            close_files();
            throw;
        }
    }

    MMapGraphStore::~MMapGraphStore() { close_files(); }

    void MMapGraphStore::close_files() noexcept {
        if (index_ != nullptr && index_bytes_ != 0) {
            ::munmap(const_cast<IndexRecord *>(index_), index_bytes_);
        }
        if (neighbors_ != nullptr && neighbor_bytes_ != 0) {
            ::munmap(const_cast<std::uint64_t *>(neighbors_), neighbor_bytes_);
        }
        index_ = nullptr;
        neighbors_ = nullptr;
        if (index_fd_ >= 0)
            ::close(index_fd_);
        if (neighbors_fd_ >= 0)
            ::close(neighbors_fd_);
        index_fd_ = neighbors_fd_ = -1;
    }

    bool MMapGraphStore::can_expand(const NodeRef &, const ExpandSpec &spec) const {
        return descriptor_.capabilities.supports_batch_neighbors && descriptor_.coverage.fresh &&
               supports_cache_spec(spec);
    }

    bool MMapGraphStore::can_pull_expand(const ExpandSpec &spec) const {
        return descriptor_.coverage.fresh && descriptor_.coverage.complete_for_requested_subgraph &&
               supports_cache_spec(spec) && index_ != nullptr;
    }

    void MMapGraphStore::stream_pull_candidates(const ExpandSpec &spec, const int depth, const BfsOptions &options,
                                                const VertexMembershipPredicate &frontier_contains,
                                                const VertexMembershipPredicate &visited_contains,
                                                const PullCandidateConsumer &consumer) {
        EVENT_SCOPE_INFO(total_span, "mmap.pull_candidates", {"depth", depth}, {"vertices", index_count_},
                         {"candidate_batch_size", options.streaming_candidate_batch_size});

        if (!consumer || index_count_ == 0) {
            return;
        }
        if (!can_pull_expand(spec)) {
            throw std::runtime_error(
                    "Memory-mapped pull expansion requires complete cache coverage and unfiltered Direction::Both");
        }
        if (!frontier_contains || !visited_contains) {
            throw std::invalid_argument("Memory-mapped pull expansion requires frontier and visited predicates");
        }
        if (options.streaming_candidate_batch_size == 0) {
            throw std::invalid_argument("streaming_candidate_batch_size must be greater than zero");
        }

        std::vector<PullCandidate> emitted;
        emitted.reserve(options.streaming_candidate_batch_size);
        std::size_t scanned_vertices = 0;
        std::size_t skipped_visited = 0;
        std::size_t inspected_edges = 0;
        std::size_t discovered_vertices = 0;

        const auto flush = [&]() {
            if (!emitted.empty()) {
                consumer(std::span<const PullCandidate>(emitted));
                emitted.clear();
            }
        };

        for (std::size_t row_index = 0; row_index < index_count_; ++row_index) {
            const IndexRecord &entry = index_[row_index];
            const GlobalId target = entry.canonical_id;
            ++scanned_vertices;
            if (visited_contains(target)) {
                ++skipped_visited;
                continue;
            }
            if (entry.neighbor_offset + entry.neighbor_count > neighbor_count_) {
                throw std::runtime_error("Corrupt mmap cache adjacency bounds during pull expansion");
            }

            for (std::uint64_t i = 0; i < entry.neighbor_count; ++i) {
                const GlobalId neighbor = neighbors_[entry.neighbor_offset + i];
                ++inspected_edges;
                if (!frontier_contains(neighbor)) {
                    continue;
                }

                emitted.push_back(PullCandidate{
                        .vertex = NodeRef{.global_id = target, .type = unresolved_type_id},
                        .predecessor = NodeRef{.global_id = neighbor, .type = unresolved_type_id},
                });
                ++discovered_vertices;
                if (emitted.size() >= options.streaming_candidate_batch_size) {
                    flush();
                }
                // Bottom-up BFS only needs one frontier neighbor.
                break;
            }
        }
        flush();

        EVENT_SCOPE_INFO_FIELD(total_span, "scanned_vertices", scanned_vertices);
        EVENT_SCOPE_INFO_FIELD(total_span, "skipped_visited", skipped_visited);
        EVENT_SCOPE_INFO_FIELD(total_span, "inspected_edges", inspected_edges);
        EVENT_SCOPE_INFO_FIELD(total_span, "discovered_vertices", discovered_vertices);
    }

    const MMapGraphStore::IndexRecord *MMapGraphStore::find(const std::uint64_t canonical_id) const noexcept {
        if (index_ == nullptr)
            return nullptr;
        const auto *first = index_;
        const auto *last = index_ + index_count_;
        const auto *it =
                std::lower_bound(first, last, canonical_id, [](const IndexRecord &record, const std::uint64_t value) {
                    return record.canonical_id < value;
                });
        return it != last && it->canonical_id == canonical_id ? it : nullptr;
    }

    std::vector<MinimalAdjacencyRow> MMapGraphStore::get_minimal_neighbors_batch(std::span<const NodeRef> frontier,
                                                                                 const ExpandSpec &spec, int depth,
                                                                                 const BfsOptions &options) {
        (void) depth;
        (void) options;
        if (!supports_cache_spec(spec)) {
            throw std::runtime_error("Memory-mapped cache supports unfiltered Direction::Both minimal expansion only");
        }
        std::vector<MinimalAdjacencyRow> rows;
        rows.reserve(frontier.size());
        for (const NodeRef &source: frontier) {
            MinimalAdjacencyRow row{.source = source, .neighbors = {}};
            const auto canonical = insertion::BackendEncodedId::canonical_id(source.global_id);
            const IndexRecord *entry = find(canonical);
            if (entry == nullptr) {
                throw std::runtime_error("mmap cache residency bit is set but the source is absent from the cache");
            }
            if (entry->neighbor_offset + entry->neighbor_count > neighbor_count_) {
                throw std::runtime_error("Corrupt mmap cache adjacency bounds");
            }
            row.neighbors.reserve(static_cast<std::size_t>(entry->neighbor_count));
            for (std::uint64_t i = 0; i < entry->neighbor_count; ++i) {
                row.neighbors.push_back(
                        NodeRef{.global_id = neighbors_[entry->neighbor_offset + i], .type = unresolved_type_id});
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    std::vector<AdjacencyRow> MMapGraphStore::get_neighbors_batch(std::span<const NodeRef> frontier,
                                                                  const ExpandSpec &spec, int depth) {
        BfsOptions options{};
        auto minimal = get_minimal_neighbors_batch(frontier, spec, depth, options);
        std::vector<AdjacencyRow> rows;
        rows.reserve(minimal.size());
        for (auto &source_row: minimal) {
            AdjacencyRow row{.source = source_row.source, .neighbors = {}};
            row.neighbors.reserve(source_row.neighbors.size());
            for (const auto &neighbor: source_row.neighbors) {
                row.neighbors.push_back(NeighborRef{.vertex = neighbor, .via_edge = std::nullopt});
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

} // namespace polygraph
