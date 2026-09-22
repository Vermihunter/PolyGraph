find_package(Doxygen REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(
    api_reference
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/generate_api_reference.py"
            --root "${PROJECT_SOURCE_DIR}"
            --output "${PROJECT_SOURCE_DIR}/docs/API_REFERENCE.md"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Regenerating the Markdown API declaration inventory"
    VERBATIM
)

set(DOXYGEN_INPUT_PATHS
    "${PROJECT_SOURCE_DIR}/docs/README.md"
    "${PROJECT_SOURCE_DIR}/docs"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)

list(JOIN DOXYGEN_INPUT_PATHS " " DOXYGEN_INPUT)

set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    @ONLY
)

add_custom_target(
    docs
    COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Generating PolyGraph HTML API documentation"
    VERBATIM
)

add_dependencies(docs api_reference)
