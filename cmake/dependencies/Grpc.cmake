include_guard(GLOBAL)

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

foreach(required_target
    protobuf::libprotobuf
    protobuf::protoc
    gRPC::grpc++
    gRPC::grpc_cpp_plugin
)
    if(NOT TARGET "${required_target}")
        message(FATAL_ERROR "The gRPC toolchain does not provide ${required_target}")
    endif()
endforeach()

message(STATUS "PolyGraph dependency: Protobuf and gRPC")
