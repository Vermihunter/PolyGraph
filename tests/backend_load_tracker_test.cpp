#include "insertion/BackendLoadTracker.hpp"

#include <gtest/gtest.h>

namespace polygraph::insertion {
    namespace {

        TEST(BackendLoadTrackerTest, UnknownBackendStartsAtZero) {
            InMemoryBackendLoadTracker tracker;

            EXPECT_EQ(tracker.load(10).owned_nodes, 0U);
            EXPECT_EQ(tracker.load(10).stored_edges, 0U);
        }

        TEST(BackendLoadTrackerTest, RecordsOwnedNodeAsStoredAndOwned) {
            InMemoryBackendLoadTracker tracker;

            tracker.record_committed(1, EntityKind::Node, 3, true);
            const BackendLoad load = tracker.load(1);

            EXPECT_EQ(load.owned_nodes, 3U);
            EXPECT_EQ(load.stored_nodes, 3U);
            EXPECT_EQ(load.owned_edges, 0U);
        }

        TEST(BackendLoadTrackerTest, RecordsReplicaNodeAsStoredOnly) {
            InMemoryBackendLoadTracker tracker;

            tracker.record_committed(2, EntityKind::Node, 4, false);
            const BackendLoad load = tracker.load(2);

            EXPECT_EQ(load.owned_nodes, 0U);
            EXPECT_EQ(load.stored_nodes, 4U);
        }

        TEST(BackendLoadTrackerTest, SeparatesNodeAndEdgeCounters) {
            InMemoryBackendLoadTracker tracker;

            tracker.record_committed(3, EntityKind::Node, 2, true);
            tracker.record_committed(3, EntityKind::Edge, 5, true);
            const BackendLoad load = tracker.load(3);

            EXPECT_EQ(load.owned_nodes, 2U);
            EXPECT_EQ(load.stored_nodes, 2U);
            EXPECT_EQ(load.owned_edges, 5U);
            EXPECT_EQ(load.stored_edges, 5U);
        }

        TEST(BackendLoadTrackerTest, SetReplacesCounters) {
            InMemoryBackendLoadTracker tracker;
            tracker.set(4, BackendLoad{.owned_nodes = 10, .stored_edges = 11});

            const BackendLoad load = tracker.load(4);
            EXPECT_EQ(load.owned_nodes, 10U);
            EXPECT_EQ(load.stored_edges, 11U);
        }

        TEST(BackendLoadSnapshotTest, ReservationAffectsSnapshotButNotTracker) {
            InMemoryBackendLoadTracker tracker;
            tracker.set(5, BackendLoad{.owned_nodes = 2});
            BackendLoadSnapshot snapshot(tracker);

            snapshot.reserve_owned(5, EntityKind::Node, 3);

            EXPECT_EQ(snapshot.load(5).owned_nodes, 5U);
            EXPECT_EQ(tracker.load(5).owned_nodes, 2U);
        }

        TEST(BackendLoadSnapshotTest, ReservationsAreKindSpecific) {
            InMemoryBackendLoadTracker tracker;
            BackendLoadSnapshot snapshot(tracker);

            snapshot.reserve_owned(6, EntityKind::Node, 2);
            snapshot.reserve_owned(6, EntityKind::Edge, 7);

            EXPECT_EQ(snapshot.load(6).owned_nodes, 2U);
            EXPECT_EQ(snapshot.load(6).owned_edges, 7U);
        }

    } // namespace
} // namespace polygraph::insertion
