# Single-source Markdown + C++ documentation site. Build output stays in build/.
find_package(Doxygen REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(POLYGRAPH_DOT_EXECUTABLE dot)
if(POLYGRAPH_DOT_EXECUTABLE)
    set(DOXYGEN_HAVE_DOT YES)
else()
    set(DOXYGEN_HAVE_DOT NO)
endif()

set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${DOXYGEN_OUTPUT_DIRECTORY}")
set(DOXYGEN_INPUT "\"${PROJECT_SOURCE_DIR}/README.md\" \"${PROJECT_SOURCE_DIR}/docs\" \"${PROJECT_SOURCE_DIR}/include\" \"${PROJECT_SOURCE_DIR}/src\" \"${PROJECT_SOURCE_DIR}/app\"")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    @ONLY
)

add_custom_target(docs_check
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/docs/check_links.py"
            "${PROJECT_SOURCE_DIR}"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Checking internal Markdown links"
    VERBATIM
)

add_custom_target(docs
    COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Building searchable PolyGraph developer wiki and C++ API reference"
    VERBATIM
)

add_dependencies(docs docs_check)
