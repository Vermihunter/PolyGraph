/**
 * @file include/data_access/NeighborBatchConsumer.hpp
 * @brief Bounded candidate-stream callback used by memory-scalable BFS expansion.
 */

#ifndef POLYGRAPH_DATA_ACCESS_NEIGHBOR_BATCH_CONSUMER_HPP
#define POLYGRAPH_DATA_ACCESS_NEIGHBOR_BATCH_CONSUMER_HPP

#include "data_model/NodeRef.hpp"

#include <functional>
#include <span>

namespace polygraph {

/**
 * @brief Consumes a transient batch of neighbor candidates.
 *
 * Implementations may invoke the callback concurrently from multiple backend or
 * request workers. The span is valid only for the duration of the call and the
 * consumer must copy any values it wants to retain.
 */
using NeighborBatchConsumer = std::function<void(std::span<const NodeRef>)>;

} // namespace polygraph

#endif // POLYGRAPH_DATA_ACCESS_NEIGHBOR_BATCH_CONSUMER_HPP
