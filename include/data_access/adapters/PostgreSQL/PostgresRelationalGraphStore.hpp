#pragma once

#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <unordered_set>
#include "data_access/IGraphStore.hpp"
#include "data_access/adapters/PostgreSQL/RelationalIdentityResolver.hpp"
#include "data_access/adapters/PostgreSQL/RelationalRelationshipRead.hpp"
#include "infrastructure/BackendConnectionConfig.hpp"

namespace polygraph {
    class PostgresRelationalGraphStore final : public IGraphStore {
    public:
        PostgresRelationalGraphStore(BackendDescriptor, BackendConnectionConfig, const domain::LogicalGraphSchema &,
                                     bool candidate_deduplication = false);
        PostgresRelationalGraphStore(const PostgresRelationalGraphStore &) = delete;
        PostgresRelationalGraphStore &operator=(const PostgresRelationalGraphStore &) = delete;
        [[nodiscard]] BackendKind kind() const override { return BackendKind::PostgreSQL; }
        [[nodiscard]] const BackendDescriptor &descriptor() const override { return descriptor_; }
        std::vector<AdjacencyRow> get_neighbors_batch(std::span<const NodeRef>, const ExpandSpec &, int) override;
        std::vector<MinimalAdjacencyRow> get_minimal_neighbors_batch(std::span<const NodeRef>, const ExpandSpec &, int,
                                                                     const BfsOptions &) override;
        void stream_neighbor_candidates(std::span<const NodeRef>, const ExpandSpec &, int, const BfsOptions &,
                                        const NeighborBatchConsumer &) override;

    private:
        using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
        struct Lease {
            explicit Lease(PostgresRelationalGraphStore &owner) : owner(owner), connection(owner.acquire()) {}
            ~Lease() { owner.release(std::move(connection)); }
            PostgresRelationalGraphStore &owner;
            Connection connection;
        };
        Connection acquire();
        void release(Connection) noexcept;
        void expand(std::span<const NodeRef>, const ExpandSpec &, const BfsOptions &,
                    const postgres::RelationalRelationshipConsumer &);
        BackendDescriptor descriptor_;
        BackendConnectionConfig config_;
        postgres::RelationalMapping mapping_;
        postgres::RelationalIdentityResolver identities_;
        bool deduplicate_;
        std::unordered_set<TypeId> node_types_, edge_types_;
        // Only the idle slot is shared; a request owns its connection exclusively.
        std::mutex idle_mutex_;
        Connection idle_{nullptr, PQfinish};
    };
} // namespace polygraph
