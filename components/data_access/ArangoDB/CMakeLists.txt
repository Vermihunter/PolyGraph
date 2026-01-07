
add_library(arango SHARED src/arango.cpp)

target_include_directories(arango PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include 
    ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(arango PRIVATE db_iface)
set_target_properties(arango PROPERTIES OUTPUT_NAME "arango")
