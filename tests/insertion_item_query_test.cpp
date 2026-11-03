#include "infrastructure/DatabaseContext.hpp"
#include "insertion/InsertionItem.hpp"
#include "query/QueryVisitor.hpp"
#include "query/query_types/InsertBatchQuery.hpp"
#include "query/query_types/InsertQuery.hpp"

#include <gtest/gtest.h>
#include <memory>

namespace polygraph {
    namespace {

        Node make_node(TypeId type = 7) {
            return Node{.global_id = GlobalId{42}, .type = type, .labels = {}, .properties = {}};
        }

        Edge make_edge(TypeId type = 8) {
            return Edge{
                    .global_id = GlobalId{73}, .type = type, .src = GlobalId{1}, .dst = GlobalId{2}, .properties = {}};
        }

        class InsertCapturingVisitor final : public QueryVisitor {
        public:
            explicit InsertCapturingVisitor(DatabaseContext &context) : QueryVisitor(context) {}

            query_result_t visit(const GetAllEdgesQuery &) override { return {}; }
            query_result_t visit(const GetAllVerticesQuery &) override { return {}; }
            query_result_t visit(const GetNeighborsForVertexQuery &) override { return {}; }
            query_result_t visit(const GetVertexByIdQuery &) override { return {}; }
            query_result_t visit(const GetNeighborsBatchQuery &) override { return {}; }

            query_result_t visit(const InsertQuery &query) override {
                observed_single = &query;
                return std::make_unique<QueryResult>();
            }

            query_result_t visit(const InsertBatchQuery &query) override {
                observed_batch = &query;
                return std::make_unique<QueryResult>();
            }

            const InsertQuery *observed_single = nullptr;
            const InsertBatchQuery *observed_batch = nullptr;
        };

        TEST(InsertionItemTest, RejectsNullNode) {
            EXPECT_THROW(insertion::InsertionItem(std::shared_ptr<const Node>{}), std::invalid_argument);
        }

        TEST(InsertionItemTest, RejectsNullEdge) {
            EXPECT_THROW(insertion::InsertionItem(std::shared_ptr<const Edge>{}), std::invalid_argument);
        }

        TEST(InsertionItemTest, ExposesNodeMetadata) {
            auto node = std::make_shared<const Node>(make_node(42));
            insertion::InsertionItem item(node);

            EXPECT_TRUE(item.is_node());
            EXPECT_FALSE(item.is_edge());
            EXPECT_EQ(item.kind(), insertion::EntityKind::Node);
            EXPECT_EQ(item.type(), 42U);
            EXPECT_EQ(item.global_id(), node->global_id);
            EXPECT_EQ(&item.node(), node.get());
            EXPECT_THROW((void) item.edge(), std::logic_error);
        }

        TEST(InsertionItemTest, ExposesEdgeMetadata) {
            auto edge = std::make_shared<const Edge>(make_edge(51));
            insertion::InsertionItem item(edge);

            EXPECT_TRUE(item.is_edge());
            EXPECT_FALSE(item.is_node());
            EXPECT_EQ(item.kind(), insertion::EntityKind::Edge);
            EXPECT_EQ(item.type(), 51U);
            EXPECT_EQ(item.global_id(), edge->global_id);
            EXPECT_EQ(&item.edge(), edge.get());
            EXPECT_THROW((void) item.node(), std::logic_error);
        }

        TEST(InsertQueryTest, CloneSharesImmutableEntity) {
            auto node = std::make_shared<const Node>(make_node());
            InsertQuery original{insertion::InsertionItem(node)};

            query_t clone = original.clone();
            const auto *typed = dynamic_cast<const InsertQuery *>(clone.get());

            ASSERT_NE(typed, nullptr);
            EXPECT_EQ(&typed->item().node(), node.get());
        }

        TEST(InsertBatchQueryTest, ReportsSizeAndClonesItems) {
            auto node = std::make_shared<const Node>(make_node());
            auto edge = std::make_shared<const Edge>(make_edge());
            InsertBatchQuery original({insertion::InsertionItem(node), insertion::InsertionItem(edge)});

            EXPECT_FALSE(original.empty());
            EXPECT_EQ(original.size(), 2U);
            query_t clone = original.clone();
            const auto *typed = dynamic_cast<const InsertBatchQuery *>(clone.get());

            ASSERT_NE(typed, nullptr);
            ASSERT_EQ(typed->items().size(), 2U);
            EXPECT_EQ(&typed->items()[0].node(), node.get());
            EXPECT_EQ(&typed->items()[1].edge(), edge.get());
        }

        TEST(InsertQueriesTest, DispatchThroughVisitor) {
            auto context = DatabaseContext::create_empty();
            InsertCapturingVisitor visitor(*context);
            InsertQuery single(insertion::InsertionItem(std::make_shared<const Node>(make_node())));
            InsertBatchQuery batch({insertion::InsertionItem(std::make_shared<const Edge>(make_edge()))});

            EXPECT_NE(single.accept(visitor), nullptr);
            EXPECT_EQ(visitor.observed_single, &single);
            EXPECT_NE(batch.accept(visitor), nullptr);
            EXPECT_EQ(visitor.observed_batch, &batch);
        }

    } // namespace
} // namespace polygraph
