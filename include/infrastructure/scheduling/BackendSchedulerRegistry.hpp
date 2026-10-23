/**
 * @file include/infrastructure/scheduling/BackendSchedulerRegistry.hpp
 * @brief Registry of server-lifetime request schedulers keyed by backend ID.
 */
#ifndef POLYGRAPH_INFRASTRUCTURE_SCHEDULING_BACKEND_SCHEDULER_REGISTRY_HPP
#define POLYGRAPH_INFRASTRUCTURE_SCHEDULING_BACKEND_SCHEDULER_REGISTRY_HPP

#include "infrastructure/BackendId.hpp"
#include "infrastructure/scheduling/BackendRequestScheduler.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace polygraph {

class BackendSchedulerRegistry final {
  public:
    [[nodiscard]] std::shared_ptr<BackendRequestScheduler>
    get_or_create(BackendId id, std::string backend_name, const BackendSchedulerConfig& config);

    [[nodiscard]] std::shared_ptr<BackendRequestScheduler> get(BackendId id) const;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<BackendId, std::shared_ptr<BackendRequestScheduler>> schedulers_;
};

} // namespace polygraph

#endif // POLYGRAPH_INFRASTRUCTURE_SCHEDULING_BACKEND_SCHEDULER_REGISTRY_HPP
