include_guard(GLOBAL)

function(polygraph_add_grpc_proto target)
    cmake_parse_arguments(ARG "" "PROTO" "" ${ARGN})
    if(NOT ARG_PROTO)
        message(FATAL_ERROR "polygraph_add_grpc_proto(${target}) requires PROTO")
    endif()

    get_filename_component(proto_file "${ARG_PROTO}" ABSOLUTE)
    get_filename_component(proto_dir "${proto_file}" DIRECTORY)
    get_filename_component(proto_name "${proto_file}" NAME_WE)
    set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")

    set(generated_sources
        "${generated_dir}/${proto_name}.pb.cc"
        "${generated_dir}/${proto_name}.grpc.pb.cc"
    )
    set(generated_headers
        "${generated_dir}/${proto_name}.pb.h"
        "${generated_dir}/${proto_name}.grpc.pb.h"
    )

    add_custom_command(
        OUTPUT ${generated_sources} ${generated_headers}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${generated_dir}"
        COMMAND protobuf::protoc
            "--proto_path=${proto_dir}"
            "--cpp_out=${generated_dir}"
            "--grpc_out=${generated_dir}"
            "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
            "${proto_file}"
        DEPENDS "${proto_file}" protobuf::protoc gRPC::grpc_cpp_plugin
        COMMENT "Generating C++ sources from ${proto_name}.proto"
        VERBATIM
    )

    set_source_files_properties(
        ${generated_sources} ${generated_headers}
        PROPERTIES GENERATED TRUE SKIP_LINTING TRUE
    )

    add_library("${target}" STATIC ${generated_sources} ${generated_headers})
    target_include_directories("${target}" PUBLIC "${generated_dir}")
    target_link_libraries("${target}" PUBLIC protobuf::libprotobuf gRPC::grpc++)
    target_compile_features("${target}" PUBLIC cxx_std_17)
endfunction()
