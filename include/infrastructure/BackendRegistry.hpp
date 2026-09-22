/**
 * @file include/infrastructure/BackendRegistry.hpp
 * @brief Registry of active graph stores and immutable backend descriptors.
 */

#ifndef POLYGRAPH_INFRASTRUCTURE_BACKEND_REGISTRY_HPP
#define POLYGRAPH_INFRASTRUCTURE_BACKEND_REGISTRY_HPP

#include "data_access/IGraphStore.hpp"

#include <memory>
#include <unordered_map>

namespace polygraph {

/**
 * @brief Owns shared graph-store instances indexed by BackendId.
 *
 * Registering the same ID again replaces both the store and descriptor. The
 * registry is normally populated during runtime construction and treated as
 * read-only while requests execute.
 */
class BackendRegistry {
  public:
    /// Mapping from backend ID to live store instance.
    using StoreMap = std::unordered_map<BackendId, std::shared_ptr<IGraphStore>>;
    /// Mapping from backend ID to copied immutable descriptor.
    using DescriptorMap = std::unordered_map<BackendId, BackendDescriptor>;

    /** @brief Registers or replaces @p store using its descriptor ID. */
    void register_store(std::shared_ptr<IGraphStore> store);

    /** @brief Returns the registered store for @p id. */
    [[nodiscard]] std::shared_ptr<IGraphStore> get_store(BackendId id) const;
    /** @brief Returns descriptor metadata for @p id. */
    [[nodiscard]] const BackendDescriptor& descriptor(BackendId id) const;
    /** @brief Returns all registered descriptor snapshots. */
    [[nodiscard]] const DescriptorMap& descriptors() const noexcept;
    /** @brief Returns all registered store instances. */
    [[nodiscard]] const StoreMap& stores() const noexcept;
    /** @brief Returns whether @p id currently names a registered store. */
    [[nodiscard]] bool contains(BackendId id) const noexcept;

  private:
    StoreMap stores_;
    DescriptorMap descriptors_;
};

} // namespace polygraph

#endif // POLYGRAPH_INFRASTRUCTURE_BACKEND_REGISTRY_HPP
