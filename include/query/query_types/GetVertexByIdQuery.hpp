//
// Created by akos on 19. 3. 2026.
//

#ifndef CPGDB_GETVERTEXBYIDQUERY_HPP
#define CPGDB_GETVERTEXBYIDQUERY_HPP


#include <query/Query.hpp>
#include <query/QueryResult.hpp>
#include <query/QueryVisitor.hpp>
#include <utilities/uuid.hpp>

class GetVertexByIdQuery : public Query {
    Node& node_;
public:
    explicit GetVertexByIdQuery(Node& node)
        : node_(node) {
    }

    [[nodiscard]] Node &getNode() const {
        return node_;
    }

    query_result_t accept(QueryVisitor& visitor) const override {
        return visitor.visit(*this);
    }

    [[nodiscard]] query_t clone() const override {
        return std::make_unique<GetVertexByIdQuery>(node_);
    }

};

#endif //CPGDB_GETVERTEXBYIDQUERY_HPP