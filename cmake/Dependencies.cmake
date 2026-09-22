include_guard(GLOBAL)

include(dependencies/Spdlog)

if(POLYGRAPH_BUILD_RUNTIME)
    include(dependencies/Argparse)
endif()

if(POLYGRAPH_ENABLE_METRICS)
    include(dependencies/PrometheusCpp)
endif()

if(POLYGRAPH_USE_JEMALLOC)
    include(dependencies/Jemalloc)
endif()

if(POLYGRAPH_BUILD_BACKENDS)
    include(dependencies/Curl)
    find_package(PostgreSQL REQUIRED)
endif()

if(POLYGRAPH_ENABLE_GPU)
    include(dependencies/Gpu)
endif()

if(POLYGRAPH_BUILD_HTTP)
    include(dependencies/Http)
endif()

if(POLYGRAPH_BUILD_GRPC)
    include(dependencies/Grpc)
endif()


