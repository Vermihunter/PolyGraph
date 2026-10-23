#include "analysis/AlgorithmAnalyzer.hpp"

#if POLYGRAPH_ENABLE_ALGORITHM_ANALYZER

#include "utilities/BackendIds.hpp"
#include "utilities/json.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace polygraph::algorithm_analysis {
    namespace {

        std::filesystem::path output_path() {
            if (const char *configured = std::getenv("POLYGRAPH_ALGORITHM_ANALYZER_OUTPUT");
                configured != nullptr && *configured != '\0') {
                return {configured};
            }
            return {"logs/polygraph.algorithm.jsonl"};
        }

    } // namespace

    AlgorithmAnalyzer &AlgorithmAnalyzer::instance() {
        static AlgorithmAnalyzer analyzer;
        return analyzer;
    }

    std::size_t AlgorithmAnalyzer::UseKeyHash::operator()(const UseKey &value) const noexcept {
        std::size_t seed = std::hash<std::string>{}(value.role);
        const auto combine = [&seed](const std::size_t next) {
            seed ^= next + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
        };
        combine(std::hash<TypeId>{}(value.type));
        combine(std::hash<BackendId>{}(value.owner));
        combine(std::hash<BackendId>{}(value.execution));
        combine(std::hash<bool>{}(value.proxy));
        return seed;
    }

    void AlgorithmAnalyzer::register_schema(const domain::LogicalGraphSchema &schema) {
        std::scoped_lock lock(mutex_);
        type_names_.clear();
        type_names_.emplace(unresolved_type_id, "<unresolved>");
        for (const auto &[type_id, type]: schema.node_types()) {
            type_names_.emplace(type_id, type.name);
        }
    }

    void AlgorithmAnalyzer::begin_query(const std::uint64_t query_id, const NodeRef &source) {
        std::scoped_lock lock(mutex_);
        QueryStats stats;
        stats.source = source.global_id;
        stats.source_type = source.type;
        queries_.insert_or_assign(query_id, std::move(stats));
    }

    void AlgorithmAnalyzer::finish_query(const std::uint64_t query_id) {
        QueryStats stats;
        {
            std::scoped_lock lock(mutex_);
            const auto iterator = queries_.find(query_id);
            if (iterator == queries_.end()) {
                return;
            }
            stats = std::move(iterator->second);
            queries_.erase(iterator);
        }
        write_query(query_id, stats);
    }

    bool AlgorithmAnalyzer::is_persistent_backend(const BackendId backend) noexcept {
        return backend == NEO4J || backend == ARANGO || backend == POSTGRESQL;
    }

    std::string AlgorithmAnalyzer::backend_name(const BackendId backend) {
        if (backend == NEO4J)
            return "Neo4J";
        if (backend == ARANGO)
            return "ArangoDB";
        if (backend == POSTGRESQL)
            return "PostgreSQL";
        if (backend == REDIS)
            return "Redis";
        if (backend == MMAP)
            return "MMap";
        if (is_gpu_backend_id(backend))
            return "GPU:" + std::to_string(gpu_device_id_from_backend(backend));
        if (backend == MEMGRAPH)
            return "Memgraph";
        return "backend:" + std::to_string(backend);
    }

    std::string AlgorithmAnalyzer::type_name(const TypeId type) const {
        if (const auto iterator = type_names_.find(type); iterator != type_names_.end()) {
            return iterator->second;
        }
        return "type:" + std::to_string(type);
    }

    void AlgorithmAnalyzer::record_one(QueryStats &stats, const std::string_view role,
                                       const BackendId execution_backend, const NodeRef &node, const BackendId owner) {
        const bool proxy = is_persistent_backend(execution_backend) && owner != execution_backend;
        UseKey key{.role = std::string(role),
                   .type = node.type,
                   .owner = owner,
                   .execution = execution_backend,
                   .proxy = proxy};
        ++stats.breakdown[key];
        if (role == "source") {
            ++stats.source_uses;
            if (proxy)
                ++stats.source_proxy_uses;
        } else {
            ++stats.destination_uses;
            if (proxy)
                ++stats.destination_proxy_uses;
        }
        if (proxy) {
            ++stats.proxy_uses;
        }
    }

    void AlgorithmAnalyzer::record_node_batch(const std::uint64_t query_id, const std::string_view role,
                                              const BackendId execution_backend, const std::span<const NodeRef> nodes,
                                              const OwnerResolver &owner_of) {
        if (nodes.empty()) {
            return;
        }
        std::scoped_lock lock(mutex_);
        const auto iterator = queries_.find(query_id);
        if (iterator == queries_.end()) {
            return;
        }
        for (const NodeRef &node: nodes) {
            record_one(iterator->second, role, execution_backend, node, owner_of(node.global_id));
        }
    }

    void AlgorithmAnalyzer::record_destination_rows(const std::uint64_t query_id, const BackendId execution_backend,
                                                    const std::span<const MinimalAdjacencyRow> rows,
                                                    const OwnerResolver &owner_of) {
        std::scoped_lock lock(mutex_);
        const auto iterator = queries_.find(query_id);
        if (iterator == queries_.end()) {
            return;
        }
        for (const MinimalAdjacencyRow &row: rows) {
            for (const NodeRef &neighbor: row.neighbors) {
                record_one(iterator->second, "destination", execution_backend, neighbor, owner_of(neighbor.global_id));
            }
        }
    }

    void AlgorithmAnalyzer::write_query(const std::uint64_t query_id, const QueryStats &stats) const {
        nlohmann::json breakdown = nlohmann::json::array();
        for (const auto &[key, count]: stats.breakdown) {
            breakdown.push_back({
                    {"role", key.role},
                    {"typeId", key.type},
                    {"type", type_name(key.type)},
                    {"ownerBackendId", key.owner},
                    {"ownerBackend", backend_name(key.owner)},
                    {"executionBackendId", key.execution},
                    {"executionBackend", backend_name(key.execution)},
                    {"proxy", key.proxy},
                    {"count", count},
            });
        }

        nlohmann::json record = {
                {"format", "polygraph.algorithm-analysis"},
                {"version", 1},
                {"queryId", query_id},
                {"sourceGlobalId", stats.source},
                {"sourceTypeId", stats.source_type},
                {"sourceType", type_name(stats.source_type)},
                {"sourceNodeUses", stats.source_uses},
                {"destinationNodeUses", stats.destination_uses},
                {"proxyNodeUses", stats.proxy_uses},
                {"sourceProxyUses", stats.source_proxy_uses},
                {"destinationProxyUses", stats.destination_proxy_uses},
                {"nodeBreakdown", std::move(breakdown)},
        };

        const auto path = output_path();
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::scoped_lock output_lock(output_mutex_);
        std::ofstream output(path, std::ios::app);
        output << record.dump() << '\n';
    }

} // namespace polygraph::algorithm_analysis

#endif
