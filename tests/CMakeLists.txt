include(dependencies/GoogleTest)
find_package(Threads REQUIRED)
include(GoogleTest)

function(polygraph_add_unit_test target_name source_file link_target)
    add_executable("${target_name}" "${source_file}")
    target_link_libraries("${target_name}" PRIVATE "${link_target}" GTest::gtest_main Threads::Threads)
    target_include_directories("${target_name}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    polygraph_enable_project_checks("${target_name}")
    gtest_discover_tests(
        "${target_name}"
        TEST_PREFIX "unit."
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS unit TIMEOUT 30
    )
endfunction()

polygraph_add_unit_test(polygraph_expand_spec_test expand_spec_test.cpp polygraph::core)
polygraph_add_unit_test(
    polygraph_data_model_json_serializer_test
    data_model_json_serializer_test.cpp
    polygraph::serialization
)
polygraph_add_unit_test(
    polygraph_record_json_serializer_test
    record_json_serializer_test.cpp
    polygraph::serialization
)
polygraph_add_unit_test(
    polygraph_dense_id_mapper_test
    dense_id_mapper_test.cpp
    polygraph::core
)
polygraph_add_unit_test(polygraph_backend_registry_test backend_registry_test.cpp polygraph::core)
polygraph_add_unit_test(
    polygraph_backend_request_scheduler_test
    backend_request_scheduler_test.cpp
    polygraph::core
)
polygraph_add_unit_test(polygraph_placement_catalog_test placement_catalog_test.cpp polygraph::core)
polygraph_add_unit_test(polygraph_bfs_executor_test bfs_executor_test.cpp polygraph::core)
polygraph_add_unit_test(polygraph_backend_encoded_uuid_test backend_encoded_uuid_test.cpp polygraph::core)
polygraph_add_unit_test(polygraph_backend_load_tracker_test backend_load_tracker_test.cpp polygraph::core)
if(TARGET polygraph_backends)
    polygraph_add_unit_test(polygraph_postgres_relational_identity_test postgres_relational_identity_test.cpp polygraph::backends)
    polygraph_add_unit_test(polygraph_postgres_relational_config_test postgres_relational_config_test.cpp polygraph::backends)
    polygraph_add_unit_test(polygraph_postgres_relational_mapping_test postgres_relational_mapping_test.cpp polygraph::backends)
    polygraph_add_unit_test(
        polygraph_redis_graph_store_test
        redis_graph_store_test.cpp
        polygraph::backends
    )
endif()

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
add_test(
    NAME python.csv_compare
    COMMAND "${Python3_EXECUTABLE}" -m unittest tools.measurement.test_csv_compare
)
set_tests_properties(python.csv_compare PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "unit;python"
    TIMEOUT 30
)
add_test(
    NAME python.reference_oracle
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s tests/reference -t . -p "test_*.py"
)
set_tests_properties(python.reference_oracle PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "unit;python;oracle"
    TIMEOUT 30
)
polygraph_add_unit_test(polygraph_insertion_item_query_test insertion_item_query_test.cpp polygraph::core)
polygraph_add_unit_test(
    polygraph_insertion_placement_policy_test
    insertion_placement_policy_test.cpp
    polygraph::core
)
polygraph_add_unit_test(
    polygraph_insertion_coordinator_test
    insertion_coordinator_test.cpp
    polygraph::core
)

if(TARGET polygraph_runtime)
    polygraph_add_unit_test(polygraph_command_line_test command_line_test.cpp polygraph::runtime)
    polygraph_add_unit_test(polygraph_public_headers_test public_headers_test.cpp polygraph::runtime)
endif()

# Phase 3 uses real core objects and a one-hop fake; no database adapters.
set(executor_fixtures "${CMAKE_CURRENT_BINARY_DIR}/executor_oracle_fixtures.json")
add_custom_command(
    OUTPUT "${executor_fixtures}"
    COMMAND "${Python3_EXECUTABLE}" -m tests.generate_executor_fixtures "${executor_fixtures}"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DEPENDS generate_executor_fixtures.py reference/bfs.py reference/model.py reference/schema.py
    VERBATIM
)
add_custom_target(polygraph_executor_oracle_fixtures DEPENDS "${executor_fixtures}")
add_library(polygraph_scripted_graph_store STATIC support/ScriptedGraphStore.cpp)
target_link_libraries(polygraph_scripted_graph_store PUBLIC polygraph::core Threads::Threads)
target_include_directories(polygraph_scripted_graph_store PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
polygraph_enable_project_checks(polygraph_scripted_graph_store)
polygraph_add_unit_test(polygraph_scripted_graph_store_test scripted_graph_store_test.cpp polygraph_scripted_graph_store)

function(polygraph_add_executor_contract_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE polygraph_scripted_graph_store GTest::gtest_main)
    target_compile_definitions(${target} PRIVATE POLYGRAPH_EXECUTOR_FIXTURES="${executor_fixtures}")
    add_dependencies(${target} polygraph_executor_oracle_fixtures)
    # Parameter names come from JSON at --gtest_list_tests time. Regenerating it
    # must relink and rerun discovery, including when case names/count change.
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${executor_fixtures}")
    polygraph_enable_project_checks(${target})
    gtest_discover_tests(${target}
        TEST_PREFIX "contract."
        NO_PRETTY_VALUES
        DISCOVERY_TIMEOUT 30
        PROPERTIES TIMEOUT 20
    )
endfunction()
polygraph_add_executor_contract_test(polygraph_bfs_contract_test bfs_contract_test.cpp)
polygraph_add_executor_contract_test(polygraph_bfs_concurrency_test bfs_concurrency_test.cpp)

# Applied after discovery: preserve list-valued labels across CMake versions.
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/executor_test_labels.cmake")

# Developer-gate regressions use executable fixtures, not compilers or git push.
add_test(NAME python.developer_quality
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s tools/dev -p "test_*.py" -v)
set_tests_properties(python.developer_quality PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "unit;python;developer"
    TIMEOUT 60
)

if(TARGET polygraph_ingestion)
    if(TARGET polygraph_runtime)
        polygraph_add_unit_test(polygraph_runtime_source_test runtime_source_test.cpp polygraph::runtime)
        target_link_libraries(polygraph_runtime_source_test PRIVATE polygraph::ingestion polygraph_scripted_graph_store)
        if(TARGET polygraph_bfs)
            target_compile_definitions(polygraph_runtime_source_test PRIVATE POLYGRAPH_TEST_BFS_BINARY="$<TARGET_FILE:polygraph_bfs>")
            add_dependencies(polygraph_runtime_source_test polygraph_bfs)
        endif()
    endif()
    polygraph_add_unit_test(polygraph_source_identity_test source_identity_test.cpp polygraph::ingestion)
    polygraph_add_unit_test(polygraph_graph_manifest_test graph_manifest_test.cpp polygraph::ingestion)
    polygraph_add_unit_test(polygraph_input_adapter_registry_test input_adapter_registry_test.cpp polygraph::ingestion)
    polygraph_add_unit_test(polygraph_ingestion_test ingestion_test.cpp polygraph::ingestion)
    polygraph_add_unit_test(polygraph_ingestion_batch_test ingestion_batch_test.cpp polygraph::ingestion)
    target_compile_definitions(polygraph_ingestion_batch_test PRIVATE _GLIBCXX_ASSERTIONS)
    polygraph_add_unit_test(polygraph_ingestion_partition_test ingestion_partition_test.cpp polygraph::ingestion)
    target_compile_definitions(polygraph_ingestion_partition_test PRIVATE
        _GLIBCXX_ASSERTIONS POLYGRAPH_TEST_HAS_KAMINPAR=$<BOOL:${POLYGRAPH_INGESTION_KAMINPAR}>)
    target_include_directories(polygraph_ingestion_partition_test PRIVATE "${PROJECT_SOURCE_DIR}/src/ingestion")
endif()
if(TARGET polygraph_bulk_import)
    add_test(NAME python.graph_tools
        COMMAND "${CMAKE_COMMAND}" -E env "POLYGRAPH_BULK_IMPORT=$<TARGET_FILE:polygraph_bulk_import>"
                "${Python3_EXECUTABLE}" -m unittest tools.test_graph_tools -v)
    set_tests_properties(python.graph_tools PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;python;ingestion" TIMEOUT 60)
    add_test(NAME python.import_workflow
        COMMAND "${CMAKE_COMMAND}" -E env "POLYGRAPH_BULK_IMPORT=$<TARGET_FILE:polygraph_bulk_import>"
                "${Python3_EXECUTABLE}" -m unittest tools.ingestion.test_import_workflow -v)
    set_tests_properties(python.import_workflow PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;python;ingestion" TIMEOUT 60)
    add_test(NAME python.graph_manifest
        COMMAND "${CMAKE_COMMAND}" -E env "POLYGRAPH_BULK_IMPORT=$<TARGET_FILE:polygraph_bulk_import>"
                "${Python3_EXECUTABLE}" -m unittest tools.ingestion.test_graph_manifest -v)
    set_tests_properties(python.graph_manifest PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;python;ingestion" TIMEOUT 60)
    add_test(NAME python.ingestion_semantics
        COMMAND "${CMAKE_COMMAND}" -E env "POLYGRAPH_BULK_IMPORT=$<TARGET_FILE:polygraph_bulk_import>"
                "${Python3_EXECUTABLE}" -m unittest tools.ingestion.test_semantics -v)
    set_tests_properties(python.ingestion_semantics PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;python;ingestion" TIMEOUT 60)
endif()
if(POLYGRAPH_INGESTION_LIVE_TESTS)
    if(NOT TARGET polygraph_bulk_import OR NOT TARGET polygraph_bfs)
        message(FATAL_ERROR "Ingestion live tests require bulk importer and CPU BFS targets")
    endif()
    foreach(adapter ldbc-snb polygraph-jsonl)
        add_test(NAME "integration.ingestion.${adapter}"
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ingestion/disposable_live.py"
                --run --unique-output --adapter "${adapter}"
                --binary "$<TARGET_FILE:polygraph_bulk_import>" --bfs "$<TARGET_FILE:polygraph_bfs>"
                --output-dir "${CMAKE_BINARY_DIR}/ingestion-live/ctest")
        set_tests_properties("integration.ingestion.${adapter}" PROPERTIES LABELS "integration;live;ingestion" TIMEOUT 300 RUN_SERIAL TRUE)
    endforeach()
endif()
if(POLYGRAPH_INGESTION_LIVE_TESTS AND TARGET polygraph_movie_import)
    add_test(NAME integration.ingestion.movie-graph
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ingestion/disposable_live.py"
            --run --unique-output --adapter movie-graph
            --binary "$<TARGET_FILE:polygraph_movie_import>" --bfs "$<TARGET_FILE:polygraph_bfs>"
            --output-dir "${CMAKE_BINARY_DIR}/ingestion-live/movie")
    set_tests_properties(integration.ingestion.movie-graph PROPERTIES
        LABELS "integration;live;ingestion" TIMEOUT 600 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
endif()
add_test(NAME python.ingestion_live_graph
    COMMAND "${Python3_EXECUTABLE}" -m unittest tools.ingestion.test_live_graph -v)
set_tests_properties(python.ingestion_live_graph PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;python;ingestion" TIMEOUT 20)
if(TARGET polygraph_bulk_import)
    add_test(NAME python.ingestion_docker_workflow
        COMMAND "${CMAKE_COMMAND}" -E env "POLYGRAPH_BULK_IMPORT=$<TARGET_FILE:polygraph_bulk_import>"
                "${Python3_EXECUTABLE}" -m unittest tools.ingestion.test_docker_workflow -v)
    set_tests_properties(python.ingestion_docker_workflow PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;python;ingestion" TIMEOUT 60)
    add_test(NAME python.ingestion_benchmark_metrics
        COMMAND "${Python3_EXECUTABLE}" -m unittest tools.ingestion.test_benchmark_ldbc tools.ingestion.test_partition_benchmark -v)
    set_tests_properties(python.ingestion_benchmark_metrics PROPERTIES WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;python;ingestion" TIMEOUT 20)
endif()
if(POLYGRAPH_INGESTION_LIVE_TESTS)
    add_test(NAME integration.ingestion.docker-load
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ingestion/docker_load_live.py"
            --run --binary "$<TARGET_FILE:polygraph_bulk_import>" --bfs "$<TARGET_FILE:polygraph_bfs>"
            --output-dir "${CMAKE_BINARY_DIR}/ingestion-docker-load/ctest")
    # Four isolated Docker lifecycles plus repeated read-only verification on potentially slow local storage.
    set_tests_properties(integration.ingestion.docker-load PROPERTIES LABELS "integration;live;ingestion" TIMEOUT 900 RUN_SERIAL TRUE)
    if(POLYGRAPH_INGESTION_KAMINPAR)
        add_test(NAME integration.ingestion.kaminpar
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ingestion/docker_load_live.py"
                --run --binary "$<TARGET_FILE:polygraph_bulk_import>" --bfs "$<TARGET_FILE:polygraph_bfs>"
                --partitioner kaminpar --cases keep canonical mapped
                --output-dir "${CMAKE_BINARY_DIR}/ingestion-partition/ctest")
        set_tests_properties(integration.ingestion.kaminpar PROPERTIES LABELS "integration;live;ingestion;kaminpar" TIMEOUT 600 RUN_SERIAL TRUE)
    endif()
endif()

polygraph_add_unit_test(polygraph_storage_type_names_test storage_type_names_test.cpp polygraph::core)
if(TARGET polygraph_backends)
    polygraph_add_unit_test(polygraph_neo4j_storage_names_test neo4j_storage_names_test.cpp polygraph::backends)
endif()
if(TARGET polygraph_runtime)
    polygraph_add_unit_test(polygraph_schema_backend_live_test schema_backend_live_test.cpp polygraph::runtime)
endif()

add_test(NAME python.schema_agnostic_cache
    COMMAND "${Python3_EXECUTABLE}" -m unittest tests.test_schema_agnostic_cache)
set_tests_properties(python.schema_agnostic_cache PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;python;cache" TIMEOUT 30)

add_test(NAME python.graph_workspace
    COMMAND "${Python3_EXECUTABLE}" -m unittest tools.graphio.test_workspace)
set_tests_properties(python.graph_workspace PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;python;workspace" TIMEOUT 30)

if(TARGET polygraph_bulk_import)
    polygraph_add_unit_test(polygraph_bulk_import_cli_test bulk_import_cli_test.cpp polygraph::ingestion_cli)
endif()
if(TARGET polygraph_movie_input)
    polygraph_add_unit_test(polygraph_movie_adapter_test movie_adapter_test.cpp polygraph_movie_input)
endif()

if(TARGET polygraph_movie_import)
    add_test(NAME python.movie_graph_example
        COMMAND "${CMAKE_COMMAND}" -E env "POLYGRAPH_MOVIE_IMPORT=$<TARGET_FILE:polygraph_movie_import>"
                "${Python3_EXECUTABLE}" -m unittest tests.test_movie_example -v)
    set_tests_properties(python.movie_graph_example PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;python;ingestion;example" TIMEOUT 90)
endif()

if(TARGET polygraph_bulk_import)
    set(adapter_contract_environment "POLYGRAPH_BULK_IMPORT=$<TARGET_FILE:polygraph_bulk_import>")
    if(TARGET polygraph_movie_import)
        list(APPEND adapter_contract_environment "POLYGRAPH_MOVIE_IMPORT=$<TARGET_FILE:polygraph_movie_import>")
    endif()
    add_test(NAME python.adapter_contracts
        COMMAND "${CMAKE_COMMAND}" -E env ${adapter_contract_environment}
                "${Python3_EXECUTABLE}" -m unittest tests.test_adapter_contract -v)
    set_tests_properties(python.adapter_contracts PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" LABELS "unit;python;ingestion;contract" TIMEOUT 90)
endif()

if(TARGET polygraph_backends)
    add_executable(polygraph_postgres_relational_metadata_test postgres_relational_metadata_test.cpp)
    target_link_libraries(polygraph_postgres_relational_metadata_test PRIVATE polygraph::backends GTest::gtest)
    polygraph_enable_project_checks(polygraph_postgres_relational_metadata_test)
    add_test(NAME integration.postgres-relational-metadata COMMAND polygraph_postgres_relational_metadata_test)
    set_tests_properties(integration.postgres-relational-metadata PROPERTIES LABELS "integration;live;postgresql" TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()

if(TARGET polygraph_runtime AND TARGET polygraph_ingestion)
    add_executable(polygraph_postgres_relational_attachment_test postgres_relational_attachment_test.cpp)
    target_link_libraries(polygraph_postgres_relational_attachment_test PRIVATE polygraph::runtime polygraph::ingestion GTest::gtest)
    polygraph_enable_project_checks(polygraph_postgres_relational_attachment_test)
    target_compile_definitions(polygraph_postgres_relational_attachment_test PRIVATE POLYGRAPH_ATTACH_POSTGRES="$<TARGET_FILE:polygraph_attach_postgres>")
    add_dependencies(polygraph_postgres_relational_attachment_test polygraph_attach_postgres)
    add_test(NAME integration.postgres-relational-identity COMMAND polygraph_postgres_relational_attachment_test)
    set_tests_properties(integration.postgres-relational-identity PROPERTIES LABELS "integration;live;postgresql" TIMEOUT 90 SKIP_RETURN_CODE 77)
endif()

if(TARGET polygraph_backends)
    add_executable(polygraph_postgres_relational_relationship_test postgres_relational_relationship_test.cpp)
    target_link_libraries(polygraph_postgres_relational_relationship_test PRIVATE polygraph::backends GTest::gtest)
    polygraph_enable_project_checks(polygraph_postgres_relational_relationship_test)
    add_test(NAME integration.postgres-relational-relationships COMMAND polygraph_postgres_relational_relationship_test)
    set_tests_properties(integration.postgres-relational-relationships PROPERTIES LABELS "integration;live;postgresql" TIMEOUT 90 SKIP_RETURN_CODE 77)
endif()

if(TARGET polygraph_backends)
    add_executable(polygraph_postgres_relational_foreign_key_test postgres_relational_foreign_key_test.cpp)
    target_link_libraries(polygraph_postgres_relational_foreign_key_test PRIVATE polygraph::backends GTest::gtest)
    polygraph_enable_project_checks(polygraph_postgres_relational_foreign_key_test)
    add_test(NAME integration.postgres-relational-foreign-keys COMMAND polygraph_postgres_relational_foreign_key_test)
    set_tests_properties(integration.postgres-relational-foreign-keys PROPERTIES LABELS "integration;live;postgresql" TIMEOUT 90 SKIP_RETURN_CODE 77)
endif()

if(TARGET polygraph_backends)
    add_executable(polygraph_postgres_relational_node_test postgres_relational_node_test.cpp)
    target_link_libraries(polygraph_postgres_relational_node_test PRIVATE polygraph::backends GTest::gtest)
    polygraph_enable_project_checks(polygraph_postgres_relational_node_test)
    add_test(NAME integration.postgres-relational-nodes COMMAND polygraph_postgres_relational_node_test)
    set_tests_properties(integration.postgres-relational-nodes PROPERTIES LABELS "integration;live;postgresql" TIMEOUT 90 SKIP_RETURN_CODE 77)
endif()

if(TARGET polygraph_ingestion AND TARGET polygraph_relational AND POLYGRAPH_INGESTION_NATIVE_EXECUTORS)
    add_executable(polygraph_postgres_relational_ingestion_test postgres_relational_ingestion_test.cpp)
    target_link_libraries(polygraph_postgres_relational_ingestion_test PRIVATE polygraph::ingestion polygraph::relational GTest::gtest)
    target_compile_definitions(polygraph_postgres_relational_ingestion_test PRIVATE POLYGRAPH_BULK_IMPORT="$<TARGET_FILE:polygraph_bulk_import>")
    add_dependencies(polygraph_postgres_relational_ingestion_test polygraph_bulk_import)
    polygraph_enable_project_checks(polygraph_postgres_relational_ingestion_test)
    add_test(NAME integration.postgres-relational-ingestion COMMAND polygraph_postgres_relational_ingestion_test)
    set_tests_properties(integration.postgres-relational-ingestion PROPERTIES LABELS "integration;live;postgresql" TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
