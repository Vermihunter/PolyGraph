#include "ingestion/BatchImport.hpp"
#include <algorithm>

namespace polygraph::ingestion {
    BoundedBatchLoader::BoundedBatchLoader(BatchImportLimits limits, Importer importer) :
        limits_(limits), importer_(std::move(importer)) {
        if (limits_.workers == 0 || limits_.workers > 32 || limits_.max_queued_batches == 0 ||
            limits_.max_buffered_bytes == 0 || !importer_)
            throw Error("Invalid bounded import worker/queue/buffer limits");
        try {
            workers_.reserve(limits_.workers);
            for (unsigned i = 0; i < limits_.workers; ++i)
                workers_.emplace_back([this] { worker(); });
        } catch (...) {
            // The class destructor does not run when thread creation fails.
            cancel();
            workers_.clear();
            throw;
        }
    }

    BoundedBatchLoader::~BoundedBatchLoader() {
        cancel();
        workers_.clear();
    }

    void BoundedBatchLoader::discard_pending() {
        for (const auto &batch: pending_)
            stats_.buffered_bytes -= batch.body.size();
        pending_.clear();
    }

    void BoundedBatchLoader::cancel() noexcept {
        {
            std::scoped_lock guard(mutex_);
            cancelled_ = true;
            discard_pending();
        }
        changed_.notify_all();
    }

    void BoundedBatchLoader::submit(ImportBatch batch) {
        const auto bytes = batch.body.size();
        if (bytes == 0 || batch.records == 0 || batch.collection.empty())
            throw Error("Import batch requires a collection, records and nonempty payload");
        if (bytes > limits_.max_buffered_bytes)
            throw Error("Import batch exceeds configured buffer budget; increase import_buffer_mb");
        std::unique_lock guard(mutex_);
        changed_.wait(guard, [&] {
            return cancelled_ || closed_ ||
                   (pending_.size() < limits_.max_queued_batches &&
                    bytes <= limits_.max_buffered_bytes - stats_.buffered_bytes);
        });
        if (error_)
            std::rethrow_exception(error_);
        if (cancelled_ || closed_)
            throw Error("Import batch loader is closed/cancelled");
        pending_.push_back(std::move(batch));
        stats_.buffered_bytes += bytes;
        stats_.peak_buffered_bytes = std::max(stats_.peak_buffered_bytes, stats_.buffered_bytes);
        stats_.peak_queued_batches = std::max(stats_.peak_queued_batches, pending_.size());
        guard.unlock();
        changed_.notify_all();
    }

    void BoundedBatchLoader::worker() {
        for (;;) {
            ImportBatch batch;
            {
                std::unique_lock guard(mutex_);
                changed_.wait(guard, [&] { return cancelled_ || closed_ || !pending_.empty(); });
                if (cancelled_ || pending_.empty())
                    return;
                batch = std::move(pending_.front());
                pending_.pop_front();
                ++stats_.active_batches;
                stats_.peak_active_batches = std::max(stats_.peak_active_batches, stats_.active_batches);
            }
            changed_.notify_all();
            std::exception_ptr failure;
            try {
                importer_(batch); // No lock around serialization, HTTP I/O or user callbacks.
            } catch (...) {
                failure = std::current_exception();
            }
            {
                std::scoped_lock guard(mutex_);
                --stats_.active_batches;
                stats_.buffered_bytes -= batch.body.size();
                if (failure) {
                    if (!error_)
                        error_ = failure;
                    cancelled_ = true;
                    discard_pending();
                } else {
                    ++stats_.completed_batches;
                    stats_.completed_records += batch.records;
                }
            }
            changed_.notify_all();
            if (failure)
                return;
        }
    }

    BatchImportStats BoundedBatchLoader::finish() {
        {
            std::scoped_lock guard(mutex_);
            closed_ = true;
        }
        changed_.notify_all();
        // Explicit join drains pending jobs; jthread destruction alone requests stop.
        for (auto &thread: workers_)
            if (thread.joinable())
                thread.join();
        if (error_)
            std::rethrow_exception(error_);
        return stats();
    }

    BatchImportStats BoundedBatchLoader::stats() const {
        std::scoped_lock guard(mutex_);
        return stats_;
    }

    std::uint64_t stream_import_batches(std::istream &input, const std::string &collection, std::size_t target_bytes,
                                        std::size_t max_record_bytes, const std::function<void(ImportBatch)> &consume) {
        constexpr std::size_t maximum_record = 64 * 1024 * 1024;
        if (target_bytes == 0 || target_bytes > maximum_record || max_record_bytes == 0 ||
            max_record_bytes > maximum_record || !consume)
            throw Error("Invalid import batch or record size limit");
        const auto capacity = max_record_bytes + 2; // Data, delimiter and terminating NUL.
        auto line = std::make_unique_for_overwrite<char[]>(capacity);
        std::string body;
        body.reserve(target_bytes);
        std::uint64_t count = 0, total = 0;
        const auto flush = [&] {
            if (count == 0)
                return;
            consume({collection, std::move(body), count});
            total += count;
            body.clear();
            body.reserve(target_bytes);
            count = 0;
        };
        for (;;) {
            input.getline(line.get(), static_cast<std::streamsize>(capacity));
            const auto extracted = static_cast<std::size_t>(input.gcount());
            if (input.bad())
                throw Error("Arango artifact read failed");
            if (extracted == 0 && input.eof())
                break;
            if (input.fail() && !input.eof())
                throw Error("Arango artifact record exceeds execution record limit");
            const auto bytes = extracted - (input.eof() ? 0 : 1);
            if (bytes > max_record_bytes)
                throw Error("Arango artifact record exceeds execution record limit");
            if (!body.empty() && body.size() + bytes + 1 > target_bytes)
                flush();
            body.append(line.get(), bytes);
            body += '\n';
            ++count;
            if (body.size() >= target_bytes)
                flush();
            if (input.eof())
                break;
        }
        flush();
        return total;
    }
} // namespace polygraph::ingestion
