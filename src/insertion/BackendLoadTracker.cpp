/**
 * @file src/insertion/BackendLoadTracker.cpp
 * @brief Implementation for backend load tracker in the insertion planning module.
 */

#include "insertion/BackendLoadTracker.hpp"

#include <utility>

namespace polygraph::insertion {

    IBackendLoadTracker::~IBackendLoadTracker() = default;

    BackendLoad InMemoryBackendLoadTracker::load(const BackendId backend_id) const {
        const std::lock_guard lock(mutex_);
        if (const auto iterator = loads_.find(backend_id); iterator != loads_.end()) {
            return iterator->second;
        }
        return {};
    }

    void InMemoryBackendLoadTracker::record_committed(const BackendId backend_id, const EntityKind kind,
                                                      const std::uint64_t count, const bool owned) {
        const std::lock_guard lock(mutex_);
        BackendLoad &load_value = loads_[backend_id];
        if (kind == EntityKind::Node) {
            load_value.stored_nodes += count;
            if (owned) {
                load_value.owned_nodes += count;
            }
        } else {
            load_value.stored_edges += count;
            if (owned) {
                load_value.owned_edges += count;
            }
        }
    }

    void InMemoryBackendLoadTracker::set(const BackendId backend_id, BackendLoad load_value) {
        const std::lock_guard lock(mutex_);
        loads_.insert_or_assign(backend_id, load_value);
    }

    BackendLoadSnapshot::BackendLoadSnapshot(const IBackendLoadTracker &tracker) : tracker_(tracker) {}

    BackendLoad BackendLoadSnapshot::load(const BackendId backend_id) const {
        if (const auto iterator = cached_.find(backend_id); iterator != cached_.end()) {
            return iterator->second;
        }
        return cached_.emplace(backend_id, tracker_.load(backend_id)).first->second;
    }

    void BackendLoadSnapshot::reserve_owned(const BackendId backend_id, const EntityKind kind,
                                            const std::uint64_t count) {
        BackendLoad load_value = load(backend_id);
        if (kind == EntityKind::Node) {
            load_value.owned_nodes += count;
        } else {
            load_value.owned_edges += count;
        }
        cached_.insert_or_assign(backend_id, load_value);
    }

} // namespace polygraph::insertion
