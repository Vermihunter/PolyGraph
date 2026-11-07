#ifndef POLYGRAPH_INGESTION_BATCH_IMPORT_HPP
#define POLYGRAPH_INGESTION_BATCH_IMPORT_HPP

#include <condition_variable>
#include <deque>
#include <istream>
#include <mutex>
#include <thread>
#include "ingestion/Import.hpp"

namespace polygraph::ingestion {
    struct ImportBatch {
        std::string collection;
        std::string body;
        std::uint64_t records = 0;
    };
    struct BatchImportLimits {
        unsigned workers = 4;
        std::size_t max_queued_batches = 8;
        // Wire-payload bytes queued OR in flight; not a whole-process RSS limit.
        std::size_t max_buffered_bytes = 128 * 1024 * 1024;
    };
    struct BatchImportStats {
        std::uint64_t completed_records = 0;
        std::uint64_t completed_batches = 0;
        std::size_t buffered_bytes = 0;
        std::size_t active_batches = 0;
        std::size_t peak_buffered_bytes = 0;
        std::size_t peak_queued_batches = 0;
        std::size_t peak_active_batches = 0;
    };

    // Single producer, fixed workers. Every callback owns its batch until its
    // acknowledgement returns. Exceptions cancel pending work; no batch is retried.
    class BoundedBatchLoader final {
    public:
        using Importer = std::function<void(const ImportBatch &)>;
        BoundedBatchLoader(BatchImportLimits limits, Importer importer);
        ~BoundedBatchLoader();
        BoundedBatchLoader(const BoundedBatchLoader &) = delete;
        BoundedBatchLoader &operator=(const BoundedBatchLoader &) = delete;
        void submit(ImportBatch batch);
        [[nodiscard]] BatchImportStats finish();
        [[nodiscard]] BatchImportStats stats() const;

    private:
        void worker();
        void cancel() noexcept;
        void discard_pending(); // mutex_ held
        const BatchImportLimits limits_;
        const Importer importer_;
        mutable std::mutex mutex_; // Guards pending_, counters, flags and error_.
        std::condition_variable changed_;
        std::deque<ImportBatch> pending_;
        BatchImportStats stats_;
        bool closed_ = false;
        bool cancelled_ = false;
        std::exception_ptr error_;
        // Destroy/join threads before any state they access.
        std::vector<std::jthread> workers_;
    };

    // Frames whole JSONL records without parsing or changing their bytes. Long
    // records form individual batches. Memory for one input record is bounded.
    [[nodiscard]] std::uint64_t stream_import_batches(std::istream &input, const std::string &collection,
                                                      std::size_t target_bytes, std::size_t max_record_bytes,
                                                      const std::function<void(ImportBatch)> &consume);
} // namespace polygraph::ingestion
#endif
