/**
 * @file src/infrastructure/scheduling/BackendSchedulerRegistry.cpp
 * @brief Implementation of backend scheduler registry.
 */
#include "infrastructure/scheduling/BackendSchedulerRegistry.hpp"

#include <stdexcept>
#include <utility>

namespace polygraph {

    std::shared_ptr<BackendRequestScheduler>
    BackendSchedulerRegistry::get_or_create(const BackendId id, std::string backend_name,
                                            const BackendSchedulerConfig &config) {
        std::scoped_lock lock(mutex_);
        if (const auto iterator = schedulers_.find(id); iterator != schedulers_.end()) {
            return iterator->second;
        }
        auto scheduler = std::make_shared<BackendRequestScheduler>(std::move(backend_name), config);
        schedulers_.emplace(id, scheduler);
        return scheduler;
    }

    std::shared_ptr<BackendRequestScheduler> BackendSchedulerRegistry::get(const BackendId id) const {
        std::scoped_lock lock(mutex_);
        const auto iterator = schedulers_.find(id);
        if (iterator == schedulers_.end()) {
            throw std::out_of_range("No request scheduler registered for backend");
        }
        return iterator->second;
    }

} // namespace polygraph
