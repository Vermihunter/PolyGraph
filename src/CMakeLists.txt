include(cmake/Model.cmake)
include(cmake/Observability.cmake)
include(cmake/Core.cmake)
include(cmake/Serialization.cmake)

if(POLYGRAPH_BUILD_BACKENDS)
    include(cmake/Backends.cmake)
endif()

if(POLYGRAPH_BUILD_RUNTIME)
    include(cmake/Runtime.cmake)
endif()

if(POLYGRAPH_BUILD_HTTP OR POLYGRAPH_BUILD_GRPC)
    include(cmake/Services.cmake)
endif()

