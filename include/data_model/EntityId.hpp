/**
 * @file include/data_model/EntityId.hpp
 * @brief Stable 64-bit identifiers used by the logical graph model.
 */

#ifndef ENTITY_ID_HPP
#define ENTITY_ID_HPP

#include <cstdint>
#include <limits>

namespace polygraph {

/**
 * Globally routed entity identifier.
 *
 * The most-significant N bits encode the owning backend. The remaining bits are
 * the backend-local identifier. Neo4j stores the local payload explicitly in
 * the integer ``localId`` property; ArangoDB and cuGraph use numeric payloads.
 */
using GlobalId = std::uint64_t;

inline constexpr GlobalId invalid_global_id = (std::numeric_limits<GlobalId>::max)();

} // namespace polygraph

#endif // ENTITY_ID_HPP
