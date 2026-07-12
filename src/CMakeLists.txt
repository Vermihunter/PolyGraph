include(cmake/Model.cmake)
include(cmake/Core.cmake)
include(cmake/Serialization.cmake)

if(CPGDB_BUILD_BACKENDS)
    include(cmake/Backends.cmake)
endif()

if(CPGDB_BUILD_RUNTIME)
    include(cmake/Runtime.cmake)
endif()

if(CPGDB_BUILD_HTTP OR CPGDB_BUILD_GRPC)
    include(cmake/Services.cmake)
endif()

