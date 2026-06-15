//
// Created by akos on 19. 3. 2026.
//

#ifndef CPGDB_GRAPHPARTITIONER_HPP
#define CPGDB_GRAPHPARTITIONER_HPP

#include <utilities/uuid.hpp>
#include "../infrastructure/BackendKind.hpp"

class GraphPartitioner {
    using uuid_range = std::pair<uuid, uuid>;

    std::vector<std::pair<uuid_range, BackendKind>> ranges;
public:
    void add_range_safe(uuid_range range, BackendKind database) {
        auto&& [new_low, new_high] = range;
        if (new_low > new_high) {
            throw std::invalid_argument{"Invalid range - low has to be lower than high"};
        }

        for (auto && [existing_range, db] : ranges) {
            auto && [existing_low, existing_high] = existing_range;

            bool existing_low_in_new_range = (existing_low >= new_low && existing_low <= new_high);
            bool existing_high_in_new_range = (existing_high >= new_low && existing_high <= new_high);

            if (existing_low_in_new_range || existing_high_in_new_range) {
                throw std::invalid_argument{"Invalid Range - ID range intersection with [" +
                    boost::uuids::to_string(existing_low) + "," + boost::uuids::to_string(existing_high)
                + "]"};
            }
        }

        ranges.emplace_back(range, database);
    }

    auto begin() {
        return ranges.begin();
    }

    auto end() {
        return ranges.end();
    }
};

using graph_partitioner_t = std::unique_ptr<GraphPartitioner>;

#endif //CPGDB_GRAPHPARTITIONER_HPP