/**
 * @file include/infrastructure/PlacementKind.hpp
 * @brief Declarations for placement kind in the backend infrastructure module.
 */

//
// Created by akos on 5. 5. 2026.
//

#ifndef CPGDB_PLACEMENTKIND_HPP
#define CPGDB_PLACEMENTKIND_HPP

namespace cpgdb {
enum class PlacementKind { Primary, Cache, GpuProjection, Replica };

} // namespace cpgdb

#endif // CPGDB_PLACEMENTKIND_HPP
