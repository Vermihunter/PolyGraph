//
// Created by akos on 19. 3. 2026.
//

#ifndef CPGDB_GETNEIGHBORSFORVERTEXQUERY_HPP
#define CPGDB_GETNEIGHBORSFORVERTEXQUERY_HPP

#include <query/Query.hpp>
#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>
#include <utilities/uuid.hpp>

class GetNeighborsForVertexQuery : public Query {
    Node& node_;
public:
    explicit GetNeighborsForVertexQuery(Node& node)
        : node_(node) {}

    Node &getNode() const {
        return node_;
    }

    query_result_t accept(QueryVisitor& visitor) const override {
        return visitor.visit(*this);
    }

    query_t clone() const override {
        return std::make_unique<GetNeighborsForVertexQuery>(node_);
    }

};

#endif //CPGDB_GETNEIGHBORSFORVERTEXQUERY_HPP