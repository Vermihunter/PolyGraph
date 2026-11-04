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

if(POLYGRAPH_BUILD_HTTP)
    include(cmake/Services.cmake)
endif()


# Opt-in standalone distributed-GPU research programs. These do not provide
# an IGraphStore backend or alter the production BFS execution path.
if(POLYGRAPH_BUILD_DISTRIBUTED_GPU_EXPERIMENTS)
    if(NOT POLYGRAPH_ENABLE_GPU)
        message(FATAL_ERROR
            "Distributed GPU research targets require POLYGRAPH_ENABLE_GPU=ON")
    endif()
    add_subdirectory(gpu/distributed)
endif()

if(POLYGRAPH_BUILD_BULK_IMPORT)
    add_subdirectory(ingestion)
endif()
