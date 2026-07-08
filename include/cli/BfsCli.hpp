//
// Created by akos on 7. 7. 2026.
//

#ifndef CPGDB_BFSCLI_HPP
#define CPGDB_BFSCLI_HPP

#include <algorithm>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include <boost/uuid/uuid_io.hpp>
#include <argparse/argparse.hpp>

#include "algorithms/bfs/BfsExecutor.hpp"
#include "domain/graph/TypeId.hpp"
#include "cli/GraphRuntimeBuilder.hpp"

namespace cpgdb::cli {

    void add_runtime_arguments(argparse::ArgumentParser& program);
    void add_bfs_arguments(argparse::ArgumentParser& program);

    [[nodiscard]] GraphRuntimeConfig parse_runtime_config(const argparse::ArgumentParser& args);

    [[nodiscard]] NodeRef parse_source(const argparse::ArgumentParser& args,
                                       const domain::LogicalGraphSchema& schema);

    [[nodiscard]] ExpandSpec parse_expand_spec(const argparse::ArgumentParser& args,
                                               const domain::LogicalGraphSchema& schema);

    [[nodiscard]] BfsOptions parse_bfs_options(const argparse::ArgumentParser& args);

    template <class BfsResultLike>
    void write_bfs_distances_csv(std::ostream& out, const BfsResultLike& result) {
        out << "vertex_id;distance\n";

        std::vector<std::pair<GlobalId, std::uint32_t>> ordered(
            result.distance.begin(),
            result.distance.end()
        );

        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            return left.second != right.second
                ? left.second < right.second
                : boost::uuids::to_string(left.first) < boost::uuids::to_string(right.first);
        });

        for (const auto& [vertex_id, distance] : ordered) {
            out << vertex_id << ';' << distance << '\n';
        }
    }

} // namespace cpgdb::cli


#endif //CPGDB_BFSCLI_HPP
