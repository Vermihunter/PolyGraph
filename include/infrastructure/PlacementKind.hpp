/**
 * @file include/infrastructure/PlacementKind.hpp
 * @brief Declarations for placement kind in the backend infrastructure module.
 */

//
// Created by akos on 5. 5. 2026.
//

#ifndef POLYGRAPH_PLACEMENTKIND_HPP
#define POLYGRAPH_PLACEMENTKIND_HPP

namespace polygraph {
enum class PlacementKind { Primary, Cache, GpuProjection, Replica };

} // namespace polygraph

#endif // POLYGRAPH_PLACEMENTKIND_HPP
