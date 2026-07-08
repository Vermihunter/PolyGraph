function(cpgdb_add_grpc_proto TARGET_NAME)
    set(options)
    set(oneValueArgs PROTO)
    set(multiValueArgs)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT ARG_PROTO)
        message(FATAL_ERROR "cpgdb_add_grpc_proto requires PROTO <file>")
    endif()

    if (NOT TARGET protobuf::libprotobuf)
        message(FATAL_ERROR "Missing target protobuf::libprotobuf. Use a proper Protobuf CONFIG package.")
    endif()

    if (NOT TARGET protobuf::protoc)
        message(FATAL_ERROR "Missing target protobuf::protoc. Use a proper Protobuf CONFIG package.")
    endif()

    if (NOT TARGET gRPC::grpc++)
        message(FATAL_ERROR "Missing target gRPC::grpc++.")
    endif()

    if (NOT TARGET gRPC::grpc_cpp_plugin)
        message(FATAL_ERROR "Missing target gRPC::grpc_cpp_plugin.")
    endif()

    get_filename_component(PROTO_ABS "${ARG_PROTO}" ABSOLUTE)
    get_filename_component(PROTO_NAME "${ARG_PROTO}" NAME_WE)
    get_filename_component(PROTO_DIR "${PROTO_ABS}" DIRECTORY)

    set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${GENERATED_DIR}")

    set(PROTO_SRC "${GENERATED_DIR}/${PROTO_NAME}.pb.cc")
    set(PROTO_HDR "${GENERATED_DIR}/${PROTO_NAME}.pb.h")
    set(GRPC_SRC "${GENERATED_DIR}/${PROTO_NAME}.grpc.pb.cc")
    set(GRPC_HDR "${GENERATED_DIR}/${PROTO_NAME}.grpc.pb.h")

    add_custom_command(
            OUTPUT
            "${PROTO_SRC}"
            "${PROTO_HDR}"
            "${GRPC_SRC}"
            "${GRPC_HDR}"
            COMMAND $<TARGET_FILE:protobuf::protoc>
            ARGS
            --proto_path=${PROTO_DIR}
            --cpp_out=${GENERATED_DIR}
            --grpc_out=${GENERATED_DIR}
            --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
            ${PROTO_ABS}
            DEPENDS "${PROTO_ABS}"
            VERBATIM
    )

    add_library(${TARGET_NAME} STATIC
            "${PROTO_SRC}"
            "${GRPC_SRC}"
    )

    target_include_directories(${TARGET_NAME} PUBLIC
            "${GENERATED_DIR}"
    )

    target_link_libraries(${TARGET_NAME} PUBLIC
            protobuf::libprotobuf
            gRPC::grpc++
    )
endfunction()