/**
 * @file src/insertion/InsertionPlacementPolicy.cpp
 * @brief Implementation for insertion placement policy in the insertion planning module.
 */

#include "insertion/InsertionPlacementPolicy.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace cpgdb::insertion {

    IInsertionPlacementPolicy::~IInsertionPlacementPolicy() = default;

    void validate_placement_decision(const PlacementDecision &decision, const BackendRegistry &backend_registry) {
        if (decision.target_backends.empty()) {
            throw std::runtime_error("Insertion placement policy selected no target backend");
        }

        std::unordered_set<BackendId> seen;
        for (const BackendId backend_id: decision.target_backends) {
            if (!backend_registry.contains(backend_id)) {
                throw std::runtime_error("Insertion placement selected an unknown backend id");
            }
            if (!seen.insert(backend_id).second) {
                throw std::runtime_error("Insertion placement selected a backend more than once");
            }
        }

        if (decision.home_backend.has_value() && !seen.contains(*decision.home_backend)) {
            throw std::runtime_error("Insertion placement home backend is not a target backend");
        }

        if (decision.encode_home_backend_in_uuid && !decision.home_backend.has_value()) {
            throw std::runtime_error("ID ownership encoding requires a home backend");
        }
    }

} // namespace cpgdb::insertion
