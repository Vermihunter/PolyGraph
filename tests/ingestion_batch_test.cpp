#include "ingestion/BatchImport.hpp"

#include <atomic>
#include <gtest/gtest.h>
#include <latch>
#include <mutex>
#include <set>
#include <sstream>

using namespace polygraph::ingestion;

TEST(IngestionBatchTest, ConcurrentDrainPreservesEveryBatchAndHonorsBounds) {
    std::latch first_two{2};
    std::atomic<unsigned> started{0};
    std::mutex result_mutex;
    std::set<std::string> received;
    BoundedBatchLoader *observed = nullptr;
    BoundedBatchLoader loader({.workers = 2, .max_queued_batches = 3, .max_buffered_bytes = 20},
                              [&](const ImportBatch &batch) {
                                  // A callback may observe metrics: it must never run under the queue mutex.
                                  EXPECT_GE(observed->stats().active_batches, 1);
                                  if (started.fetch_add(1) < 2) {
                                      first_two.count_down();
                                      first_two.wait();
                                  }
                                  std::scoped_lock guard(result_mutex);
                                  EXPECT_TRUE(received.insert(batch.collection).second);
                                  EXPECT_EQ(batch.body, "data");
                              });
    observed = &loader;
    for (unsigned i = 0; i < 100; ++i)
        loader.submit({std::to_string(i), "data", 1});
    const auto result = loader.finish();
    EXPECT_EQ(received.size(), 100);
    EXPECT_EQ(result.completed_records, 100);
    EXPECT_EQ(result.completed_batches, 100);
    EXPECT_EQ(result.peak_active_batches, 2);
    EXPECT_LE(result.peak_queued_batches, 3);
    EXPECT_LE(result.peak_buffered_bytes, 20);
    EXPECT_EQ(result.buffered_bytes, 0);
    EXPECT_EQ(result.active_batches, 0);
    EXPECT_THROW(loader.submit({"late", "data", 1}), Error);
}

TEST(IngestionBatchTest, WorkerFailureWakesBlockedProducerAndIsNotRetried) {
    std::atomic<unsigned> calls{0};
    BoundedBatchLoader loader({.workers = 1, .max_queued_batches = 1, .max_buffered_bytes = 4},
                              [&](const ImportBatch &) {
                                  ++calls;
                                  throw Error("native import acknowledgement failed");
                              });
    try {
        for (unsigned i = 0; i < 100; ++i)
            loader.submit({"Post", "data", 1});
        (void) loader.finish();
        FAIL() << "Worker failure was swallowed";
    } catch (const Error &error) {
        EXPECT_STREQ(error.what(), "native import acknowledgement failed");
    }
    EXPECT_THROW((void) loader.finish(), Error);
    EXPECT_EQ(calls, 1);
}

TEST(IngestionBatchTest, ProducerFailureCancelsAndJoinsWaitingWorkers) {
    std::atomic<unsigned> calls{0};
    EXPECT_THROW(
            {
                BoundedBatchLoader loader({.workers = 4, .max_queued_batches = 2, .max_buffered_bytes = 32},
                                          [&](const ImportBatch &) { ++calls; });
                throw Error("cannot read artifact");
            },
            Error);
    EXPECT_EQ(calls, 0);
}

TEST(IngestionBatchTest, ProducerFailureJoinsActiveRequestsAndDiscardsQueuedBatches) {
    std::latch active{1}, queued{1};
    std::atomic<unsigned> calls{0}, finished{0};
    BoundedBatchLoader *observed = nullptr;
    try {
        BoundedBatchLoader loader({.workers = 1, .max_queued_batches = 2, .max_buffered_bytes = 12},
                                  [&](const ImportBatch &) {
                                      ++calls;
                                      active.count_down();
                                      queued.wait();
                                      // Producer unwinding cancels and discards the queued four-byte job.
                                      // The active request must still finish before loader destruction returns.
                                      while (observed->stats().buffered_bytes != 4)
                                          std::this_thread::yield();
                                      ++finished;
                                  });
        observed = &loader;
        loader.submit({"active", "data", 1});
        active.wait();
        loader.submit({"pending", "data", 1});
        queued.count_down();
        throw Error("producer read failure");
    } catch (const Error &error) {
        EXPECT_STREQ(error.what(), "producer read failure");
    }
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(finished, 1);
}

TEST(IngestionBatchTest, OversizedBatchFailsImmediatelyInsteadOfWaitingForever) {
    BoundedBatchLoader loader({.workers = 1, .max_queued_batches = 1, .max_buffered_bytes = 4},
                              [](const ImportBatch &) {});
    EXPECT_THROW(loader.submit({"Post", "large", 1}), Error);
    EXPECT_EQ(loader.finish().completed_batches, 0);
}

TEST(IngestionBatchTest, InvalidLimitsFailBeforeCreatingWorkers) {
    EXPECT_THROW((BoundedBatchLoader({.workers = 0}, [](const ImportBatch &) {})), Error);
    EXPECT_THROW((BoundedBatchLoader({.max_queued_batches = 0}, [](const ImportBatch &) {})), Error);
    EXPECT_THROW((BoundedBatchLoader({.max_buffered_bytes = 0}, [](const ImportBatch &) {})), Error);
}

TEST(IngestionBatchTest, JsonlFramingPreservesEscapingUnicodeAndUnterminatedTail) {
    std::istringstream input("{\"text\":\"Ž|\\n\\\"\"}\r\n{\"id\":2}");
    std::vector<ImportBatch> batches;
    EXPECT_EQ(stream_import_batches(input, "Post", 12, 64,
                                    [&](ImportBatch batch) { batches.push_back(std::move(batch)); }),
              2);
    ASSERT_EQ(batches.size(), 2);
    EXPECT_EQ(batches[0].body, "{\"text\":\"Ž|\\n\\\"\"}\r\n");
    EXPECT_EQ(batches[1].body, "{\"id\":2}\n");
    for (const auto &batch: batches) {
        EXPECT_EQ(batch.collection, "Post");
        EXPECT_EQ(batch.records, 1);
    }
}

TEST(IngestionBatchTest, LongRecordIsKeptWholeAndRecordLimitIsEnforcedDuringRead) {
    std::istringstream accepted(std::string(16, 'x') + '\n');
    std::vector<ImportBatch> batches;
    EXPECT_EQ(stream_import_batches(accepted, "Post", 4, 16,
                                    [&](ImportBatch batch) { batches.push_back(std::move(batch)); }),
              1);
    ASSERT_EQ(batches.size(), 1);
    EXPECT_EQ(batches[0].body, std::string(16, 'x') + '\n');
    std::istringstream rejected(std::string(17, 'x') + '\n');
    EXPECT_THROW((void) stream_import_batches(rejected, "Post", 4, 16, [](ImportBatch) {}), Error);
    std::istringstream empty;
    EXPECT_EQ(stream_import_batches(empty, "Post", 4, 16, [](ImportBatch) { ADD_FAILURE(); }), 0);
}
