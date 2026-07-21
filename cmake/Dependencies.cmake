include_guard(GLOBAL)

include(dependencies/Spdlog)

if(CPGDB_BUILD_BACKENDS)
    include(dependencies/Curl)
endif()

if(CPGDB_ENABLE_GPU)
    include(dependencies/Gpu)
endif()

if(CPGDB_BUILD_HTTP)
    include(dependencies/Http)
endif()

if(CPGDB_BUILD_GRPC)
    include(dependencies/Grpc)
endif()


