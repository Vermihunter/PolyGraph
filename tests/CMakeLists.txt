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
