#pragma once

#include "query/Query.hpp"
#include "query/QueryResult.hpp"
#include "query/query_types/InsertBatchQuery.hpp"
#include "support/TestGraphStore.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

namespace polygraph::test {

    class CapturingInsertStore final : public IGraphStore {
    public:
        explicit CapturingInsertStore(BackendDescriptor descriptor, bool fail_writes = false) :
            descriptor_(std::move(descriptor)), fail_writes_(fail_writes) {}

        [[nodiscard]] BackendKind kind() const override { return descriptor_.kind; }

        [[nodiscard]] const BackendDescriptor &descriptor() const override { return descriptor_; }

        std::vector<AdjacencyRow> get_neighbors_batch(std::span<const NodeRef>, const ExpandSpec &, int) override {
            return {};
        }

        query_result_t execute(const Query &query) override {
            if (fail_writes_) {
                throw std::runtime_error("configured write failure");
            }
            const auto *batch = dynamic_cast<const InsertBatchQuery *>(&query);
            if (batch == nullptr) {
                throw std::runtime_error("CapturingInsertStore expected InsertBatchQuery");
            }
            batches_.push_back(batch->items());
            return std::make_unique<QueryResult>();
        }

        [[nodiscard]] const std::vector<std::vector<insertion::InsertionItem>> &batches() const { return batches_; }

        [[nodiscard]] std::size_t inserted_count() const {
            std::size_t result = 0;
            for (const auto &batch: batches_) {
                result += batch.size();
            }
            return result;
        }

    private:
        BackendDescriptor descriptor_;
        bool fail_writes_ = false;
        std::vector<std::vector<insertion::InsertionItem>> batches_;
    };

} // namespace polygraph::test
