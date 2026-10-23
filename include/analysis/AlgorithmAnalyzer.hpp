/**
 * @file include/analysis/AlgorithmAnalyzer.hpp
 * @brief Compile-time optional per-query algorithm statistics.
 */
#ifndef POLYGRAPH_ALGORITHM_ANALYZER_HPP
#define POLYGRAPH_ALGORITHM_ANALYZER_HPP

#ifndef POLYGRAPH_ENABLE_ALGORITHM_ANALYZER
#define POLYGRAPH_ENABLE_ALGORITHM_ANALYZER 0
#endif

#if POLYGRAPH_ENABLE_ALGORITHM_ANALYZER

#include "data_model/MinimalAdjacencyRow.hpp"
#include "domain/graph/LogicalGraphSchema.hpp"
#include "infrastructure/BackendId.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace polygraph::algorithm_analysis {

/**
 * Records algorithm-level node traffic, independently from normal events/logging.
 *
 * A "proxy use" is an occurrence where a persistent backend (Neo4j, ArangoDB,
 * PostgreSQL) serves an endpoint whose authoritative GlobalId owner is another
 * backend. This matches the loader's physical Proxy-node semantics. Cache/GPU
 * execution never counts as a proxy use because those tiers do not materialize
 * Proxy vertices.
 */
class AlgorithmAnalyzer {
  public:
    using OwnerResolver = std::function<BackendId(GlobalId)>;

    static AlgorithmAnalyzer& instance();

    void register_schema(const domain::LogicalGraphSchema& schema);
    void begin_query(std::uint64_t query_id, const NodeRef& source);
    void finish_query(std::uint64_t query_id);

    void record_node_batch(std::uint64_t query_id,
                           std::string_view role,
                           BackendId execution_backend,
                           std::span<const NodeRef> nodes,
                           const OwnerResolver& owner_of);

    void record_destination_rows(std::uint64_t query_id,
                               BackendId execution_backend,
                               std::span<const MinimalAdjacencyRow> rows,
                               const OwnerResolver& owner_of);

  private:
    struct UseKey {
        std::string role;
        TypeId type{};
        BackendId owner{};
        BackendId execution{};
        bool proxy{};

        bool operator==(const UseKey&) const = default;
    };

    struct UseKeyHash {
        std::size_t operator()(const UseKey& value) const noexcept;
    };

    struct QueryStats {
        GlobalId source{};
        TypeId source_type{unresolved_type_id};
        std::uint64_t source_uses{};
        std::uint64_t destination_uses{};
        std::uint64_t proxy_uses{};
        std::uint64_t source_proxy_uses{};
        std::uint64_t destination_proxy_uses{};
        std::unordered_map<UseKey, std::uint64_t, UseKeyHash> breakdown;
    };

    static bool is_persistent_backend(BackendId backend) noexcept;
    static std::string backend_name(BackendId backend);
    std::string type_name(TypeId type) const;
    static void record_one(QueryStats& stats,
                    std::string_view role,
                    BackendId execution_backend,
                    const NodeRef& node,
                    BackendId owner);
    void write_query(std::uint64_t query_id, const QueryStats& stats) const;

    mutable std::mutex mutex_;
    mutable std::mutex output_mutex_;
    std::unordered_map<TypeId, std::string> type_names_;
    std::unordered_map<std::uint64_t, QueryStats> queries_;
};

class ScopedAlgorithmQuery {
  public:
    ScopedAlgorithmQuery(std::uint64_t query_id, const NodeRef& source)
        : query_id_(query_id) {
        AlgorithmAnalyzer::instance().begin_query(query_id_, source);
    }

    ~ScopedAlgorithmQuery() {
        AlgorithmAnalyzer::instance().finish_query(query_id_);
    }

    ScopedAlgorithmQuery(const ScopedAlgorithmQuery&) = delete;
    ScopedAlgorithmQuery& operator=(const ScopedAlgorithmQuery&) = delete;

  private:
    std::uint64_t query_id_{};
};

} // namespace polygraph::algorithm_analysis

#define ALGORITHM_ANALYZER_QUERY_SCOPE(name, query_id, source) \
    ::polygraph::algorithm_analysis::ScopedAlgorithmQuery name((query_id), (source))
#define ALGORITHM_ANALYZER_REGISTER_SCHEMA(schema) \
    ::polygraph::algorithm_analysis::AlgorithmAnalyzer::instance().register_schema((schema))

#else

#define ALGORITHM_ANALYZER_QUERY_SCOPE(name, query_id, source) ((void)0)
#define ALGORITHM_ANALYZER_REGISTER_SCHEMA(schema) ((void)0)

#endif

#endif // POLYGRAPH_ALGORITHM_ANALYZER_HPP
