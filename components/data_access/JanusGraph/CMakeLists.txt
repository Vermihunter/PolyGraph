
add_library(janus SHARED src/janus.cpp)

target_include_directories(janus PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include 
    ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(janus PRIVATE db_iface)
set_target_properties(janus PROPERTIES OUTPUT_NAME "janus")
