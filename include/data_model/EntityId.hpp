/**
 * @file include/data_model/EntityId.hpp
 * @brief Stable 64-bit identifiers used by the logical graph model.
 */

#ifndef ENTITY_ID_HPP
#define ENTITY_ID_HPP

#include <cstdint>

namespace cpgdb {

/**
 * Globally routed entity identifier.
 *
 * The most-significant N bits encode the owning backend. The remaining bits are
 * the backend-local identifier. Neo4j uses its native node/relationship ID as
 * the local payload; ArangoDB and cuGraph use generated numeric payloads.
 */
using GlobalId = std::uint64_t;

inline constexpr GlobalId invalid_global_id = 0;

} // namespace cpgdb

#endif // ENTITY_ID_HPP
