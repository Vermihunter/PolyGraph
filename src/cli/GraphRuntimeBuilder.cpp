/**
 * @file src/cli/GraphRuntimeBuilder.cpp
 * @brief Implementation for graph runtime builder in the command-line and runtime configuration
 * module.
 */

//
// Created by akos on 7. 7. 2026.
//

#include "cli/GraphRuntimeBuilder.hpp"
#include "cli/NativeNodeIdResolver.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "logging/Log.hpp"
#include "utilities/uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef POLYGRAPH_ENABLE_GPU
#include <cuda_runtime_api.h>
#endif

#include "cli/BackendConfigParser.hpp"
#include "cli/GraphDataReader.hpp"
#include "domain/graph/TypeId.hpp"
#include "infrastructure/BackendId.hpp"
#include "infrastructure/ConfiguredGraphStoreFactory.hpp"
#include "infrastructure/PlacementCatalog.hpp"
#include "utilities/BackendIds.hpp"
#include "utilities/json.hpp"

using json = nlohmann::json;

namespace polygraph::cli {
    namespace {

        using ProjectionBuildResult = ConfiguredGraphStoreFactory::GpuProjectionInput;

        struct LoadedSchema {
            std::unique_ptr<domain::LogicalGraphSchema> schema;
            std::unordered_map<BackendId, std::unordered_set<TypeId>> nodes_by_backend;
            std::unordered_map<BackendId, std::unordered_set<TypeId>> edges_by_backend;
        };

        [[nodiscard]] const json &required_array(const json &object, std::string_view key) {
            const auto it = object.find(std::string(key));
            if (it == object.end() || !it->is_array()) {
                throw std::runtime_error("Expected JSON array field: " + std::string(key));
            }
            return *it;
        }

        [[nodiscard]] json read_json_file(const std::string &path) {
            std::ifstream input(path);
            if (!input) {
                throw std::runtime_error("Could not open JSON file: " + path);
            }
            return json::parse(input);
        }


        [[nodiscard]] std::optional<std::string>
        apply_gpu_storage_plan_runtime_backends(json &backend_root,
                                                const std::optional<std::string> &backend_config_path) {
            const auto plan_entry = backend_root.find("gpuStoragePlan");
            if (plan_entry == backend_root.end() || !plan_entry->is_string() ||
                plan_entry->get<std::string>().empty()) {
                return std::nullopt;
            }

            std::filesystem::path plan_path{plan_entry->get<std::string>()};
            if (plan_path.is_relative()) {
                if (!backend_config_path || backend_config_path->empty()) {
                    throw std::runtime_error("Relative gpuStoragePlan requires a backend configuration file path");
                }
                plan_path = std::filesystem::path(*backend_config_path).parent_path() / plan_path;
            }
            plan_path = std::filesystem::weakly_canonical(plan_path);
            const json plan = read_json_file(plan_path.string());
            if (plan.value("format", std::string{}) != "polygraph.gpu-storage-plan" || plan.value("version", 0) != 1) {
                throw std::runtime_error("Unsupported GPU storage plan: " + plan_path.string());
            }

            const auto runtime_backends = plan.find("runtimeBackends");
            if (runtime_backends == plan.end() || !runtime_backends->is_array()) {
                throw std::runtime_error("GPU storage plan is missing runtimeBackends");
            }
            auto backends_it = backend_root.find("backends");
            if (backends_it == backend_root.end() || !backends_it->is_array()) {
                throw std::runtime_error("Backend config must contain a backends array");
            }

            for (const auto &raw_backend: *runtime_backends) {
                json backend = raw_backend;
                if (!backend.is_object()) {
                    throw std::runtime_error("GPU storage runtimeBackends entries must be objects");
                }
                const auto id = backend.value("id", std::string{});
                if (id.empty()) {
                    throw std::runtime_error("GPU storage runtime backend is missing id");
                }
                if (const auto artifact = backend.find("projection_artifact");
                    artifact != backend.end() && artifact->is_string()) {
                    std::filesystem::path artifact_path{artifact->get<std::string>()};
                    if (artifact_path.is_relative()) {
                        artifact_path = plan_path.parent_path() / artifact_path;
                    }
                    backend["projection_artifact"] = std::filesystem::weakly_canonical(artifact_path).string();
                }

                bool replaced = false;
                for (auto &existing: *backends_it) {
                    if (existing.value("id", std::string{}) == id) {
                        existing = backend;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    backends_it->push_back(backend);
                }
            }
            LOG_INFO("Runtime load: applied {} authoritative GPU backend entries from {}", runtime_backends->size(),
                     plan_path.string());
            return plan_path.string();
        }

        void apply_cache_plan_runtime_backends(json &backend_root, const std::optional<std::string> &cache_plan_path) {
            if (!cache_plan_path || cache_plan_path->empty()) {
                return;
            }
            const std::filesystem::path plan_path = std::filesystem::weakly_canonical(*cache_plan_path);
            const json plan = read_json_file(plan_path.string());
            const auto runtime_backends = plan.find("runtimeBackends");
            if (runtime_backends == plan.end() || !runtime_backends->is_array()) {
                return;
            }
            auto backends_it = backend_root.find("backends");
            if (backends_it == backend_root.end() || !backends_it->is_array()) {
                throw std::runtime_error("Backend config must contain a backends array");
            }

            for (const auto &raw_cache_backend: *runtime_backends) {
                json cache_backend = raw_cache_backend;
                if (!cache_backend.is_object()) {
                    throw std::runtime_error("cache-plan runtimeBackends entries must be objects");
                }
                const auto id = cache_backend.value("id", std::string{});
                if (id.empty()) {
                    throw std::runtime_error("cache-plan runtime backend is missing id");
                }
                if (id == "Redis") {
                    cache_backend["cache_plan_path"] = plan_path.string();
                }
                if (const auto artifact = cache_backend.find("projection_artifact");
                    artifact != cache_backend.end() && artifact->is_string()) {
                    std::filesystem::path artifact_path{artifact->get<std::string>()};
                    if (artifact_path.is_relative()) {
                        artifact_path = plan_path.parent_path() / artifact_path;
                    }
                    cache_backend["projection_artifact"] = std::filesystem::weakly_canonical(artifact_path).string();
                }
                bool replaced = false;
                for (auto &existing: *backends_it) {
                    if (existing.value("id", std::string{}) != id) {
                        continue;
                    }

                    // A physical GPU can contain both authoritative storage and
                    // extra cached vertices. Keep one CuGraph store and let the
                    // projection loader merge the two independent artifacts.
                    if (existing.value("kind", std::string{}) == "CuGraph" &&
                        cache_backend.value("kind", std::string{}) == "CuGraph") {
                        const auto combine_limit = [](const std::uint64_t left, const std::uint64_t right) {
                            if (left == 0U || right == 0U) {
                                return std::uint64_t{0};
                            }
                            if (right > std::numeric_limits<std::uint64_t>::max() - left) {
                                return std::uint64_t{0};
                            }
                            return left + right;
                        };
                        existing["priority"] =
                                std::min(existing.value("priority", 0), cache_backend.value("priority", 0));
                        existing["memory_limit_bytes"] =
                                combine_limit(existing.value("memory_limit_bytes", std::uint64_t{0}),
                                              cache_backend.value("memory_limit_bytes", std::uint64_t{0}));
                        existing["relation_limit"] =
                                combine_limit(existing.value("relation_limit", std::uint64_t{0}),
                                              cache_backend.value("relation_limit", std::uint64_t{0}));
                        if (cache_backend.contains("projection_artifact")) {
                            existing["cache_projection_artifact"] = cache_backend["projection_artifact"];
                        }
                        existing["cache_role"] = "acceleration";
                    } else {
                        existing = cache_backend;
                    }
                    replaced = true;
                    break;
                }
                if (!replaced) {
                    backends_it->push_back(cache_backend);
                }
            }
            LOG_INFO("Runtime load: applied {} cache backend overrides from {}", runtime_backends->size(),
                     plan_path.string());
        }

        [[nodiscard]] std::vector<CacheResidencyRecord>
        load_cache_residency(const std::optional<std::string> &cache_plan_path, const std::string &graph_data_path) {
            std::vector<CacheResidencyRecord> residency;
            if (!cache_plan_path || cache_plan_path->empty()) {
                return residency;
            }

            const std::filesystem::path plan_path = std::filesystem::weakly_canonical(*cache_plan_path);
            const json plan = read_json_file(plan_path.string());

            if (const auto graph_entry = plan.find("graph"); graph_entry != plan.end() && graph_entry->is_string()) {
                std::filesystem::path planned_graph{graph_entry->get<std::string>()};
                if (planned_graph.is_relative()) {
                    planned_graph = plan_path.parent_path() / planned_graph;
                }
                const auto expected_graph = std::filesystem::weakly_canonical(graph_data_path);
                const auto actual_graph = std::filesystem::weakly_canonical(planned_graph);
                if (actual_graph != expected_graph) {
                    throw std::runtime_error(
                            "Cache plan was generated for a different graph: " + actual_graph.string() +
                            " (runtime graph: " + expected_graph.string() + ")");
                }
            }

            const auto artifacts = plan.find("artifacts");
            if (artifacts == plan.end() || !artifacts->is_object()) {
                return residency;
            }
            const auto residency_entry = artifacts->find("residency");
            if (residency_entry == artifacts->end() || !residency_entry->is_object()) {
                return residency;
            }
            const auto raw_path = residency_entry->value("path", std::string{});
            if (raw_path.empty()) {
                return residency;
            }
            std::filesystem::path path{raw_path};
            if (path.is_relative()) {
                path = plan_path.parent_path() / path;
            }
            path = std::filesystem::weakly_canonical(path);
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) {
                throw std::runtime_error("Could not open cache residency file: " + path.string());
            }

            static_assert(sizeof(CacheResidencyRecord) == 16);
            static_assert(std::is_trivially_copyable_v<CacheResidencyRecord>);
            const auto bytes = input.tellg();
            if (bytes < 0 || static_cast<std::uint64_t>(bytes) % sizeof(CacheResidencyRecord) != 0U) {
                throw std::runtime_error("Cache residency file has an invalid size: " + path.string());
            }
            const auto record_count =
                    static_cast<std::size_t>(static_cast<std::uint64_t>(bytes) / sizeof(CacheResidencyRecord));
            const auto expected_entries = residency_entry->value("entries", std::size_t{0});
            if (expected_entries != 0 && record_count != expected_entries) {
                throw std::runtime_error("Cache residency entry count does not match cache-plan.json");
            }

            // The on-disk record is exactly CacheResidencyRecord. Read it directly
            // into the final flat index: no temporary vector, per-record allocation
            // or hash-table construction.
            residency.resize(record_count);
            input.seekg(0);
            if (!residency.empty() &&
                !input.read(reinterpret_cast<char *>(residency.data()),
                            static_cast<std::streamsize>(residency.size() * sizeof(CacheResidencyRecord)))) {
                throw std::runtime_error("Could not read cache residency file: " + path.string());
            }

            GlobalId previous{};
            bool have_previous = false;
            for (const auto &record: residency) {
                const auto canonical_id = record.canonical_id;
                const auto mask = static_cast<insertion::BackendEncodedId::LocationMask>(record.mask);
                if (static_cast<std::uint64_t>(mask) != record.mask ||
                    (mask & ~insertion::BackendEncodedId::cache_location_mask()) != 0U) {
                    throw std::runtime_error("Cache residency contains a non-cache location mask");
                }
                if (insertion::BackendEncodedId::canonical_id(canonical_id) != canonical_id) {
                    throw std::runtime_error("Cache residency contains a non-canonical GlobalId");
                }
                if (have_previous && canonical_id <= previous) {
                    throw std::runtime_error("Cache residency must contain sorted unique canonical GlobalIds");
                }
                previous = canonical_id;
                have_previous = true;
            }
            LOG_INFO("Loaded cache plan residency index: {} entries ({} bytes) from {}", residency.size(),
                     residency.size() * sizeof(CacheResidencyRecord), path.string());
            return residency;
        }

        [[nodiscard]] std::vector<GpuOwnerRecord>
        load_gpu_owner_distribution(const std::optional<std::string> &gpu_storage_plan_path,
                                    const std::string &graph_data_path) {
            std::vector<GpuOwnerRecord> ownership;
            if (!gpu_storage_plan_path || gpu_storage_plan_path->empty()) {
                return ownership;
            }

            const std::filesystem::path plan_path = std::filesystem::weakly_canonical(*gpu_storage_plan_path);
            const json plan = read_json_file(plan_path.string());

            if (const auto graph_entry = plan.find("graph"); graph_entry != plan.end() && graph_entry->is_string()) {
                std::filesystem::path planned_graph{graph_entry->get<std::string>()};
                if (planned_graph.is_relative()) {
                    planned_graph = plan_path.parent_path() / planned_graph;
                }
                const auto expected_graph = std::filesystem::weakly_canonical(graph_data_path);
                const auto actual_graph = std::filesystem::weakly_canonical(planned_graph);
                if (actual_graph != expected_graph) {
                    throw std::runtime_error(
                            "GPU storage plan was generated for a different graph: " + actual_graph.string() +
                            " (runtime graph: " + expected_graph.string() + ")");
                }
            }

            const auto artifacts = plan.find("artifacts");
            if (artifacts == plan.end() || !artifacts->is_object()) {
                return ownership;
            }
            const auto entry = artifacts->find("ownership");
            if (entry == artifacts->end() || !entry->is_object()) {
                return ownership;
            }
            const auto raw_path = entry->value("path", std::string{});
            if (raw_path.empty()) {
                return ownership;
            }
            std::filesystem::path path{raw_path};
            if (path.is_relative()) {
                path = plan_path.parent_path() / path;
            }
            path = std::filesystem::weakly_canonical(path);

            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) {
                throw std::runtime_error("Could not open GPU ownership distribution: " + path.string());
            }
            static_assert(sizeof(GpuOwnerRecord) == 16);
            static_assert(std::is_trivially_copyable_v<GpuOwnerRecord>);
            const auto bytes = input.tellg();
            if (bytes < 0 || static_cast<std::uint64_t>(bytes) % sizeof(GpuOwnerRecord) != 0U) {
                throw std::runtime_error("GPU ownership distribution has an invalid size: " + path.string());
            }
            const auto count = static_cast<std::size_t>(static_cast<std::uint64_t>(bytes) / sizeof(GpuOwnerRecord));
            const auto expected_entries = entry->value("entries", std::size_t{0});
            if (expected_entries != 0U && count != expected_entries) {
                throw std::runtime_error("GPU ownership entry count does not match gpu-storage-plan.json");
            }

            ownership.resize(count);
            input.seekg(0);
            if (!ownership.empty() &&
                !input.read(reinterpret_cast<char *>(ownership.data()),
                            static_cast<std::streamsize>(ownership.size() * sizeof(GpuOwnerRecord)))) {
                throw std::runtime_error("Could not read GPU ownership distribution: " + path.string());
            }

            GlobalId previous{};
            bool have_previous = false;
            for (const auto &record: ownership) {
                if (record.reserved != 0U) {
                    throw std::runtime_error("GPU ownership distribution contains unsupported reserved bits");
                }
                if (record.device_id >= insertion::BackendEncodedId::MAX_GPU_DEVICES) {
                    throw std::runtime_error("GPU ownership distribution contains an unencodable device id");
                }
                if (insertion::BackendEncodedId::canonical_id(record.canonical_id) != record.canonical_id) {
                    throw std::runtime_error("GPU ownership distribution contains a non-canonical GlobalId");
                }
                if (have_previous && record.canonical_id <= previous) {
                    throw std::runtime_error(
                            "GPU ownership distribution must contain sorted unique canonical GlobalIds");
                }
                previous = record.canonical_id;
                have_previous = true;
            }
            LOG_INFO("Loaded authoritative GPU ownership index: {} entries ({} bytes) from {}", ownership.size(),
                     ownership.size() * sizeof(GpuOwnerRecord), path.string());
            return ownership;
        }

        [[nodiscard]] GlobalId json_global_id(const json &value) {
            if (value.is_number_unsigned()) {
                return value.get<GlobalId>();
            }
            if (value.is_number_integer()) {
                return global_id_from_signed_storage(value.get<std::int64_t>());
            }
            if (value.is_string()) {
                return global_id_from_string(value.get<std::string>());
            }
            throw std::runtime_error("Global ID must be an integer or decimal string");
        }

        [[nodiscard]] domain::PropertyType property_type_from_string(const std::string &value) {
            static const std::unordered_map<std::string, domain::PropertyType> by_name{
                    {"string", domain::PropertyType::string},   {"integer", domain::PropertyType::integer},
                    {"boolean", domain::PropertyType::boolean}, {"float", domain::PropertyType::_float},
                    {"date", domain::PropertyType::date},       {"datetime", domain::PropertyType::datetime},
            };

            const auto it = by_name.find(value);
            if (it == by_name.end()) {
                throw std::runtime_error("Unknown property type: " + value);
            }
            return it->second;
        }

        [[nodiscard]] std::vector<domain::Property> parse_properties(const json &properties_json) {
            std::vector<domain::Property> properties;

            for (const auto &property_json: properties_json) {
                properties.push_back(domain::Property{
                        .name = property_json.at("name").get<std::string>(),
                        .type = property_type_from_string(property_json.at("type").get<std::string>())});
            }

            return properties;
        }

        void ensure_cuda_or_throw(const unsigned int device_id) {
#ifdef POLYGRAPH_ENABLE_GPU
            int device_count = 0;

            cudaError_t error = cudaGetDeviceCount(&device_count);
            if (error != cudaSuccess) {
                throw std::runtime_error(std::string("cudaGetDeviceCount failed: ") + cudaGetErrorString(error));
            }
            if (device_count == 0) {
                throw std::runtime_error("No CUDA devices available");
            }
            if (device_id >= static_cast<unsigned int>(device_count)) {
                throw std::runtime_error("Configured NVIDIA GPU id " + std::to_string(device_id) +
                                         " is not available (device count=" + std::to_string(device_count) + ")");
            }

            error = cudaSetDevice(static_cast<int>(device_id));
            if (error != cudaSuccess) {
                throw std::runtime_error(std::string("cudaSetDevice failed: ") + cudaGetErrorString(error));
            }

            error = cudaFree(nullptr);
            if (error != cudaSuccess) {
                throw std::runtime_error(std::string("CUDA context initialization failed: ") +
                                         cudaGetErrorString(error));
            }
#else
            (void) device_id;
            throw std::runtime_error(
                    "GPU backend requested, but this build was configured with POLYGRAPH_ENABLE_GPU=OFF");
#endif
        }

        [[nodiscard]] std::uint64_t residency_mask_for(const std::vector<CacheResidencyRecord> &residency,
                                                       const GlobalId canonical) {
            const auto it = std::ranges::lower_bound(residency, canonical, {}, &CacheResidencyRecord::canonical_id);
            return it != residency.end() && it->canonical_id == canonical ? it->mask : 0U;
        }

        template<typename T>
        [[nodiscard]] std::vector<T> read_binary_vector(const std::filesystem::path &path,
                                                        const std::size_t expected_count,
                                                        const std::string_view label) {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) {
                throw std::runtime_error("Could not open " + std::string(label) + ": " + path.string());
            }
            const auto end = input.tellg();
            if (end < 0) {
                throw std::runtime_error("Could not determine size of " + std::string(label) + ": " + path.string());
            }
            const auto bytes = static_cast<std::uint64_t>(end);
            if (bytes % sizeof(T) != 0U) {
                throw std::runtime_error(std::string(label) + " has a truncated binary record: " + path.string());
            }
            const auto count = static_cast<std::size_t>(bytes / sizeof(T));
            if (count != expected_count) {
                throw std::runtime_error(std::string(label) + " record count does not match manifest");
            }
            std::vector<T> values(count);
            input.seekg(0);
            if (!values.empty() && !input.read(reinterpret_cast<char *>(values.data()),
                                               static_cast<std::streamsize>(values.size() * sizeof(T)))) {
                throw std::runtime_error("Could not read " + std::string(label) + ": " + path.string());
            }
            return values;
        }

        [[nodiscard]] std::optional<ProjectionBuildResult>
        load_prebuilt_gpu_projection(const std::optional<std::string> &plan_path_value, const unsigned int device_id) {
            if (!plan_path_value || plan_path_value->empty()) {
                return std::nullopt;
            }
            const std::filesystem::path plan_path = std::filesystem::weakly_canonical(*plan_path_value);
            const json plan = read_json_file(plan_path.string());
            const auto artifacts = plan.find("artifacts");
            if (artifacts == plan.end() || !artifacts->is_object()) {
                return std::nullopt;
            }
            const auto gpu_artifacts = artifacts->find("gpu");
            if (gpu_artifacts == artifacts->end() || !gpu_artifacts->is_object()) {
                return std::nullopt; // backward-compatible old cache plan
            }
            const auto key = std::to_string(device_id);
            const auto entry = gpu_artifacts->find(key);
            if (entry == gpu_artifacts->end() || !entry->is_object()) {
                return std::nullopt;
            }
            const auto raw_manifest = entry->value("manifest", std::string{});
            if (raw_manifest.empty()) {
                throw std::runtime_error("GPU projection artifact entry has no manifest path for device " + key);
            }
            std::filesystem::path manifest_path{raw_manifest};
            if (manifest_path.is_relative()) {
                manifest_path = plan_path.parent_path() / manifest_path;
            }
            manifest_path = std::filesystem::weakly_canonical(manifest_path);
            const json manifest = read_json_file(manifest_path.string());
            if (manifest.value("format", std::string{}) != "polygraph.gpu-projection.dense-edgelist" ||
                manifest.value("version", 0) != 1) {
                throw std::runtime_error("Unsupported GPU projection artifact: " + manifest_path.string());
            }
            if (manifest.value("deviceId", device_id) != device_id) {
                throw std::runtime_error("GPU projection artifact deviceId does not match requested device");
            }
            const auto vertex_count = manifest.value("vertices", std::size_t{0});
            const auto adjacency_pairs = manifest.value("adjacencyPairs", std::size_t{0});
            if (vertex_count == 0) {
                throw std::runtime_error("GPU projection artifact contains no vertices");
            }
            if (vertex_count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
                adjacency_pairs > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                throw std::runtime_error("GPU projection artifact exceeds int32 cuGraph limits");
            }

            const auto resolve = [&](const char *field) {
                const auto raw = manifest.value(field, std::string{});
                if (raw.empty()) {
                    throw std::runtime_error(std::string("GPU projection manifest is missing ") + field);
                }
                std::filesystem::path path{raw};
                if (path.is_relative()) {
                    path = manifest_path.parent_path() / path;
                }
                return std::filesystem::weakly_canonical(path);
            };

            const auto started = std::chrono::steady_clock::now();
            LOG_INFO("GPU:{} loading: reading reusable dense projection artifact {}", device_id,
                     manifest_path.string());
            ProjectionBuildResult result;
            result.vertex_ids =
                    read_binary_vector<GlobalId>(resolve("globalIds"), vertex_count, "GPU dense GlobalId map");
            result.dense_sources =
                    read_binary_vector<std::int32_t>(resolve("sources"), adjacency_pairs, "GPU dense source array");
            result.dense_destinations = read_binary_vector<std::int32_t>(resolve("destinations"), adjacency_pairs,
                                                                         "GPU dense destination array");
            result.resident_vertex_count = manifest.value("residentVertices", std::size_t{0});
            result.prebuilt_dense = true;
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            LOG_INFO("GPU:{} loading: reusable projection read in {:.3f}s ({} projection vertices, {} adjacency pairs)",
                     device_id, elapsed, vertex_count, adjacency_pairs);
            return result;
        }

        [[nodiscard]] ProjectionBuildResult merge_prebuilt_gpu_projections(ProjectionBuildResult base,
                                                                           ProjectionBuildResult cache) {
            if (!base.prebuilt_dense || !cache.prebuilt_dense) {
                throw std::runtime_error(
                        "Authoritative and cache GPU projections must both use the reusable dense format");
            }
            if (base.dense_sources.size() != base.dense_destinations.size() ||
                cache.dense_sources.size() != cache.dense_destinations.size()) {
                throw std::runtime_error("Reusable GPU projection has mismatched dense edge arrays");
            }

            ProjectionBuildResult result;
            result.prebuilt_dense = true;
            result.resident_vertex_count = base.resident_vertex_count + cache.resident_vertex_count;
            result.vertex_ids.reserve(base.vertex_ids.size() + cache.vertex_ids.size());
            std::ranges::set_union(base.vertex_ids, cache.vertex_ids, std::back_inserter(result.vertex_ids));

            if (result.vertex_ids.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                throw std::runtime_error("Merged GPU projection exceeds int32 dense vertex limits");
            }

            const auto remap = [&result](const std::vector<GlobalId> &ids) {
                std::vector<std::int32_t> mapping;
                mapping.reserve(ids.size());
                std::size_t merged_index = 0;
                for (const GlobalId id: ids) {
                    while (merged_index < result.vertex_ids.size() && result.vertex_ids[merged_index] < id) {
                        ++merged_index;
                    }
                    if (merged_index == result.vertex_ids.size() || result.vertex_ids[merged_index] != id) {
                        throw std::runtime_error("Merged GPU projection lost a dense GlobalId");
                    }
                    mapping.push_back(static_cast<std::int32_t>(merged_index));
                }
                return mapping;
            };
            const auto base_remap = remap(base.vertex_ids);
            const auto cache_remap = remap(cache.vertex_ids);

            const auto edge_at = [](const ProjectionBuildResult &projection, const std::vector<std::int32_t> &mapping,
                                    const std::size_t index) {
                const auto source = projection.dense_sources[index];
                const auto destination = projection.dense_destinations[index];
                if (source < 0 || destination < 0 || static_cast<std::size_t>(source) >= mapping.size() ||
                    static_cast<std::size_t>(destination) >= mapping.size()) {
                    throw std::runtime_error("Reusable GPU projection contains an invalid dense vertex id");
                }
                return std::pair{mapping[static_cast<std::size_t>(source)],
                                 mapping[static_cast<std::size_t>(destination)]};
            };

            result.dense_sources.reserve(base.dense_sources.size() + cache.dense_sources.size());
            result.dense_destinations.reserve(base.dense_destinations.size() + cache.dense_destinations.size());
            std::size_t base_index = 0;
            std::size_t cache_index = 0;
            std::optional<std::pair<std::int32_t, std::int32_t>> previous;
            const auto append_unique = [&result, &previous](const std::pair<std::int32_t, std::int32_t> edge) {
                if (previous && *previous == edge) {
                    return;
                }
                result.dense_sources.push_back(edge.first);
                result.dense_destinations.push_back(edge.second);
                previous = edge;
            };

            while (base_index < base.dense_sources.size() || cache_index < cache.dense_sources.size()) {
                if (base_index == base.dense_sources.size()) {
                    append_unique(edge_at(cache, cache_remap, cache_index++));
                    continue;
                }
                if (cache_index == cache.dense_sources.size()) {
                    append_unique(edge_at(base, base_remap, base_index++));
                    continue;
                }

                const auto base_edge = edge_at(base, base_remap, base_index);
                const auto cache_edge = edge_at(cache, cache_remap, cache_index);
                if (base_edge < cache_edge) {
                    append_unique(base_edge);
                    ++base_index;
                } else if (cache_edge < base_edge) {
                    append_unique(cache_edge);
                    ++cache_index;
                } else {
                    append_unique(base_edge);
                    ++base_index;
                    ++cache_index;
                }
            }

            return result;
        }

        [[nodiscard]] ProjectionBuildResult
        build_gpu_projection_inputs(const std::string &filename, const std::vector<BackendId> &routing_backends,
                                    const std::vector<CacheResidencyRecord> &external_residency,
                                    const bool external_cache_plan_enabled, const unsigned int device_id) {
            (void) routing_backends;
            ProjectionBuildResult result;
            const auto gpu_bit = insertion::BackendEncodedId::gpu_location_bit(device_id);
            const auto started = std::chrono::steady_clock::now();
            auto last_report = started;
            std::size_t nodes_seen = 0;
            std::size_t relationships_seen = 0;

            const auto mask_for = [&](const GlobalId id) {
                // External plans supersede legacy cache-residency bits embedded in
                // graph IDs.  The immutable owner bit is still relevant because a
                // GPU may itself be the authoritative owner of a vertex.
                auto mask = external_cache_plan_enabled ? insertion::BackendEncodedId::owner_location(id)
                                                        : insertion::BackendEncodedId::location_mask(id);
                const auto canonical = insertion::BackendEncodedId::canonical_id(id);
                mask |= static_cast<insertion::BackendEncodedId::LocationMask>(
                        residency_mask_for(external_residency, canonical));
                return mask;
            };
            const auto maybe_log = [&] {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_report >= std::chrono::seconds(5)) {
                    LOG_INFO("GPU:{} projection scan: {} nodes, {} relationships, {} selected edges", device_id,
                             nodes_seen, relationships_seen, result.edges.size());
                    last_report = now;
                }
            };

            GraphDataReader::for_each(
                    filename,
                    [&](const json &node_json) {
                        ++nodes_seen;
                        const GlobalId global_id = json_global_id(node_json.at("uuid"));
                        const GlobalId canonical = insertion::BackendEncodedId::canonical_id(global_id);
                        if ((mask_for(global_id) & gpu_bit) != 0U) {
                            result.vertex_ids.push_back(canonical);
                        }
                        maybe_log();
                    },
                    [&](const json &relationship_json) {
                        ++relationships_seen;
                        const GlobalId from_id = json_global_id(relationship_json.at("from"));
                        const GlobalId to_id = json_global_id(relationship_json.at("to"));
                        const bool gpu_resident =
                                (mask_for(from_id) & gpu_bit) != 0U || (mask_for(to_id) & gpu_bit) != 0U;
                        if (gpu_resident) {
                            const auto canonical_from = insertion::BackendEncodedId::canonical_id(from_id);
                            const auto canonical_to = insertion::BackendEncodedId::canonical_id(to_id);
                            // GpuProjectionBuilder adds edge endpoints to its dense
                            // ID input itself.  Avoid inserting them here a second
                            // time: on large projections this materially reduces
                            // host allocations, copies and sort work.
                            result.edges.emplace_back(canonical_from, canonical_to);
                        }
                        maybe_log();
                    });

            LOG_INFO("GPU:{} projection scan complete: {} nodes, {} relationships, {} selected edges", device_id,
                     nodes_seen, relationships_seen, result.edges.size());
            return result;
        }

        [[nodiscard]] LoadedSchema load_logical_graph_schema(const std::string &filename) {
            const json config = read_json_file(filename);

            LoadedSchema result;
            result.schema = std::make_unique<domain::LogicalGraphSchema>();

            // Keep your existing synthetic type.
            result.schema->add_node_type("Proxy", {});

            for (const auto &node_json: required_array(config, "nodeTypes")) {
                const auto name = node_json.at("name").get<std::string>();
                result.schema->add_node_type(name, parse_properties(node_json.value("properties", json::array())));
            }

            for (const auto &relation_json: required_array(config, "relations")) {
                const auto name = relation_json.at("name").get<std::string>();
                const TypeId from_id = result.schema->node_type_id(relation_json.at("from").get<std::string>());
                const TypeId to_id = result.schema->node_type_id(relation_json.at("to").get<std::string>());
                result.schema->add_edge_type(name, from_id, to_id, relation_json.value("directed", true),
                                             parse_properties(relation_json.value("properties", json::array())));
            }

            return result;
        }


        void validate_graph_id_layout(const std::string &filename, const std::vector<BackendId> &routing_backends,
                                      const std::optional<std::string> &cache_plan_path,
                                      const std::optional<std::string> &gpu_storage_plan_path) {
            json metadata;
            std::string metadata_source;
            const auto try_plan = [&](const std::optional<std::string> &path, const std::string_view source) {
                if (!path || path->empty() || !metadata.empty()) {
                    return;
                }
                const json plan = read_json_file(std::filesystem::weakly_canonical(*path).string());
                if (const auto graph_routing = plan.find("graphRouting");
                    graph_routing != plan.end() && graph_routing->is_object() && graph_routing->contains("idRouting")) {
                    metadata = *graph_routing;
                    metadata_source = source;
                }
            };
            try_plan(cache_plan_path, "cache plan");
            try_plan(gpu_storage_plan_path, "GPU storage plan");
            if (metadata.empty()) {
                metadata = GraphDataReader::metadata(filename);
                metadata_source = "canonical graph";
            }

            const auto layout = metadata.find("idRouting");
            if (layout == metadata.end()) {
                throw std::runtime_error(
                        "Graph/cache metadata must declare idRouting; regenerate or migrate uint64 IDs first");
            }
            if (!layout->is_array()) {
                throw std::runtime_error("Graph/cache metadata field 'idRouting' must be an array");
            }

            std::vector<BackendId> graph_routing;
            graph_routing.reserve(layout->size());
            for (const auto &value: *layout) {
                graph_routing.push_back(config::backend_id_from_string(value.get<std::string>()));
            }
            if (graph_routing != routing_backends) {
                throw std::runtime_error(
                        "Graph/cache idRouting metadata does not match backend config; ID MSB prefixes would route to "
                        "the wrong storage layer");
            }

            if (const auto bits = metadata.find("idRoutingBits");
                bits != metadata.end() && !bits->is_null() &&
                bits->get<std::size_t>() != insertion::BackendEncodedId::bit_count(routing_backends.size())) {
                throw std::runtime_error("Graph/cache idRoutingBits metadata does not match backend count");
            }
            if (metadata_source != "canonical graph") {
                LOG_INFO("Runtime load: validated ID routing from {}; canonical graph data was not opened",
                         metadata_source);
            }
        }


    } // namespace

    std::shared_ptr<DatabaseContext> GraphRuntimeBuilder::build(const GraphRuntimeConfig &config) {
        auto ctx = DatabaseContext::create_empty();

        if (config.native_id_map_path && !config.native_id_map_path->empty()) {
            auto resolver = std::make_shared<NativeNodeIdResolver>(*config.native_id_map_path);
            ctx->input_id_resolver = [resolver = std::move(resolver)](const GlobalId id) {
                return resolver->resolve(id);
            };
        }

        LOG_INFO("Runtime load: reading logical graph schema {}", config.logical_graph_schema_path);
        LoadedSchema loaded_schema = load_logical_graph_schema(config.logical_graph_schema_path);

        ctx->schema = std::move(loaded_schema.schema);
        ctx->current_graph_version = config.graph_version;

        LOG_INFO("Runtime load: reading backend configuration");
        json backend_root = config::load_backend_config_file_or_default(config.backend_config_path);
        const auto gpu_storage_plan_path =
                apply_gpu_storage_plan_runtime_backends(backend_root, config.backend_config_path);
        apply_cache_plan_runtime_backends(backend_root, config.cache_plan_path);
        const json &backend_entries = config::backend_array_or_throw(backend_root);
        ctx->id_routing_backends = config::id_routing_backends(backend_root);
        validate_graph_id_layout(config.data_path, ctx->id_routing_backends, config.cache_plan_path,
                                 gpu_storage_plan_path);
        ctx->placement_catalog->set_id_routing_backends(ctx->id_routing_backends);

        auto gpu_ownership = load_gpu_owner_distribution(gpu_storage_plan_path, config.data_path);
        auto gpu_ownership_storage = std::make_shared<std::vector<GpuOwnerRecord>>(std::move(gpu_ownership));
        if (!gpu_ownership_storage->empty()) {
            std::shared_ptr<const std::vector<GpuOwnerRecord>> ownership_shared = gpu_ownership_storage;
            ctx->placement_catalog->set_gpu_owner_distribution(std::move(ownership_shared));
        }

        const bool external_cache_plan_enabled = config.cache_plan_path && !config.cache_plan_path->empty();
        auto residency = load_cache_residency(config.cache_plan_path, config.data_path);
        auto residency_storage = std::make_shared<std::vector<CacheResidencyRecord>>(std::move(residency));
        std::shared_ptr<const std::vector<CacheResidencyRecord>> residency_shared = residency_storage;
        if (external_cache_plan_enabled) {
            ctx->placement_catalog->set_cache_residency(residency_shared);
        }

        const auto projection_builder = [routing_backends = ctx->id_routing_backends, residency_shared,
                                         external_cache_plan_enabled, cache_plan_path = config.cache_plan_path,
                                         gpu_storage_plan_path](const std::string &path, const unsigned int device_id) {
            auto authoritative = load_prebuilt_gpu_projection(gpu_storage_plan_path, device_id);
            auto cache = load_prebuilt_gpu_projection(cache_plan_path, device_id);
            if (authoritative && cache) {
                LOG_INFO("GPU:{} loading: merging authoritative storage and acceleration-cache projections", device_id);
                return merge_prebuilt_gpu_projections(std::move(*authoritative), std::move(*cache));
            }
            if (authoritative) {
                LOG_INFO("GPU:{} loading: using reusable authoritative storage projection", device_id);
                return std::move(*authoritative);
            }
            if (cache) {
                LOG_INFO("GPU:{} loading: using reusable acceleration-cache projection", device_id);
                return std::move(*cache);
            }

            if (gpu_storage_plan_path) {
                throw std::runtime_error("GPU:" + std::to_string(device_id) +
                                         " has no reusable projection in either the GPU storage plan or cache plan");
            }

            LOG_INFO("GPU:{} loading: legacy domain has no GPU storage plan; falling back to graph scan", device_id);
            return build_gpu_projection_inputs(path, routing_backends, *residency_shared, external_cache_plan_enabled,
                                               device_id);
        };

        ConfiguredGraphStoreFactory::Inputs factory_inputs{.data_path = config.data_path,
                                                           .schema = *ctx->schema,
                                                           .nodes_by_backend = loaded_schema.nodes_by_backend,
                                                           .edges_by_backend = loaded_schema.edges_by_backend,
                                                           .graph_version = ctx->current_graph_version,
                                                           .build_gpu_projection_inputs = projection_builder,
                                                           .ensure_cuda = ensure_cuda_or_throw};

        std::size_t enabled_backend_count = 0;
        for (const auto &backend_json: backend_entries) {
            const auto backend_name = backend_json.value("id", std::string{"unknown"});
            LOG_INFO("Runtime load: initializing backend {}", backend_name);
            auto store = ConfiguredGraphStoreFactory::create_from_json(*ctx, backend_json, factory_inputs);
            if (!store) {
                continue;
            }

            ctx->register_store(std::move(store));
            ++enabled_backend_count;
            LOG_INFO("Runtime load: backend {} ready", backend_name);
        }

        if (enabled_backend_count == 0) {
            throw std::runtime_error("Backend config enables no backends");
        }

        if (config.verbose) {
            config::print_backend_summary(*ctx, std::cerr);
        }


        return ctx;
    }

} // namespace polygraph::cli
