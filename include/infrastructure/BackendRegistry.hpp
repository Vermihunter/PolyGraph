//
// Created by akos on 7. 5. 2026.
//

#ifndef CPGDB_BACKENDREGISTRY_HPP
#define CPGDB_BACKENDREGISTRY_HPP
#include "data_access/IGraphStore.hpp"

class BackendRegistry {
public:
    void register_store(std::shared_ptr<IGraphStore> store) {
        auto descriptor = store->descriptor();

        descriptors_[descriptor.id] = descriptor;
        stores_[descriptor.id] = std::move(store);
    }

    std::shared_ptr<IGraphStore> get_store(BackendId id) const {
        auto it = stores_.find(id);

        if (it == stores_.end()) {
            throw std::runtime_error("Unknown backend store id");
        }

        return it->second;
    }

    const BackendDescriptor& descriptor(BackendId id) const {
        return descriptors_.at(id);
    }

    const std::unordered_map<BackendId, BackendDescriptor>& descriptors() const {
        return descriptors_;
    }

    const auto& get_stores() const {
        return stores_;
    }

    bool contains(BackendId id) const {
        return stores_.contains(id);
    }

private:
    std::unordered_map<BackendId, std::shared_ptr<IGraphStore>> stores_;
    std::unordered_map<BackendId, BackendDescriptor> descriptors_;
};

#endif //CPGDB_BACKENDREGISTRY_HPP