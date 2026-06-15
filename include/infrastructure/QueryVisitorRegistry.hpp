//
// Created by akos on 7. 5. 2026.
//

#ifndef CPGDB_QUERYVISITORREGISTRY_HPP
#define CPGDB_QUERYVISITORREGISTRY_HPP

#include "BackendId.hpp"
#include "query/QueryVisitor.hpp"

class QueryVisitorRegistry {
public:
    void register_visitor(BackendId backend_id,
                          std::shared_ptr<QueryVisitor> visitor) {
        visitors_[backend_id] = std::move(visitor);
    }

    [[nodiscard]] std::shared_ptr<QueryVisitor> get(BackendId backend_id) const {
        auto it = visitors_.find(backend_id);

        if (it == visitors_.end()) {
            throw std::runtime_error("No QueryVisitor registered for backend");
        }

        return it->second;
    }

    bool contains(BackendId backend_id) const {
        return visitors_.contains(backend_id);
    }

private:
    std::unordered_map<BackendId, std::shared_ptr<QueryVisitor>> visitors_;
};

#endif //CPGDB_QUERYVISITORREGISTRY_HPP