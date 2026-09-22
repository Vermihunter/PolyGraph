/**
 * @file src/infrastructure/BackendRegistry.cpp
 * @brief Implementation for backend registry in the backend infrastructure module.
 */

#include "infrastructure/BackendRegistry.hpp"

#include <stdexcept>
#include <utility>

namespace polygraph {

    void BackendRegistry::register_store(std::shared_ptr<IGraphStore> store) {
        if (!store) {
            throw std::invalid_argument("Cannot register a null graph store");
        }

        const BackendDescriptor descriptor_value = store->descriptor();
        descriptors_.insert_or_assign(descriptor_value.id, descriptor_value);
        stores_.insert_or_assign(descriptor_value.id, std::move(store));
    }

    std::shared_ptr<IGraphStore> BackendRegistry::get_store(BackendId id) const {
        if (const auto iterator = stores_.find(id); iterator != stores_.end()) {
            return iterator->second;
        }
        throw std::runtime_error("Unknown backend store id");
    }

    const BackendDescriptor &BackendRegistry::descriptor(BackendId id) const { return descriptors_.at(id); }

    const BackendRegistry::DescriptorMap &BackendRegistry::descriptors() const noexcept { return descriptors_; }

    const BackendRegistry::StoreMap &BackendRegistry::stores() const noexcept { return stores_; }

    bool BackendRegistry::contains(BackendId id) const noexcept { return stores_.contains(id); }

} // namespace polygraph
