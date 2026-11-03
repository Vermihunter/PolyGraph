/**
 * @file include/insertion/BackendLoadTracker.hpp
 * @brief Load metrics and projected reservations used by insertion placement policies.
 */

#ifndef POLYGRAPH_INSERTION_BACKEND_LOAD_TRACKER_HPP
#define POLYGRAPH_INSERTION_BACKEND_LOAD_TRACKER_HPP

#include "infrastructure/BackendId.hpp"
#include "insertion/InsertionItem.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace polygraph::insertion {

    /** @brief Logical ownership and physical storage counters for one backend. */
    struct BackendLoad {
        std::uint64_t owned_nodes = 0; ///< Nodes for which this backend is the logical home.
        std::uint64_t owned_edges = 0; ///< Edges for which this backend is the logical home.
        std::uint64_t stored_nodes = 0; ///< Node documents physically stored here, including replicas.
        std::uint64_t stored_edges = 0; ///< Edge documents physically stored here, including replicas.
    };

    /** @brief Injectable source and sink for durable or in-memory backend load metrics. */
    class IBackendLoadTracker {
    public:
        virtual ~IBackendLoadTracker();

        /** @brief Returns the latest committed counters for @p backend_id. */
        [[nodiscard]] virtual BackendLoad load(BackendId backend_id) const = 0;
        /** @brief Records a successful committed write. */
        virtual void record_committed(BackendId backend_id, EntityKind kind, std::uint64_t count, bool owned) = 0;
    };

    /** @brief Thread-safe process-local load tracker suitable for tests and one-process runtimes. */
    class InMemoryBackendLoadTracker final : public IBackendLoadTracker {
    public:
        [[nodiscard]] BackendLoad load(BackendId backend_id) const override;
        void record_committed(BackendId backend_id, EntityKind kind, std::uint64_t count, bool owned) override;
        /** @brief Replaces all counters for @p backend_id. */
        void set(BackendId backend_id, BackendLoad load);

    private:
        mutable std::mutex mutex_;
        std::unordered_map<BackendId, BackendLoad> loads_;
    };

    /**
     * @brief Per-planning-batch view that overlays projected ownership reservations.
     *
     * The snapshot prevents a least-loaded selector from choosing the same backend
     * repeatedly while earlier decisions in the same batch are not yet committed.
     */
    class BackendLoadSnapshot {
    public:
        explicit BackendLoadSnapshot(const IBackendLoadTracker &tracker);

        /** @brief Returns committed counters plus reservations made through this snapshot. */
        [[nodiscard]] BackendLoad load(BackendId backend_id) const;
        /** @brief Adds a projected logical ownership reservation. */
        void reserve_owned(BackendId backend_id, EntityKind kind, std::uint64_t count = 1);

    private:
        const IBackendLoadTracker &tracker_;
        mutable std::unordered_map<BackendId, BackendLoad> cached_;
    };

} // namespace polygraph::insertion

#endif // POLYGRAPH_INSERTION_BACKEND_LOAD_TRACKER_HPP
