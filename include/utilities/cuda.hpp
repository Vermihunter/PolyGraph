//
// Created by akos on 28. 5. 2026.
//

#ifndef CPGDB_CUDA_HPP
#define CPGDB_CUDA_HPP

#include <cuda_runtime.h>
#include <stdexcept>


inline void cuda_check(const cudaError_t err, const char* where) {
    if (err != cudaSuccess) {
        throw std::runtime_error(
            std::string("CUDA error at ") +
            where +
            ": " +
            cudaGetErrorString(err)
        );
    }
}


#endif //CPGDB_CUDA_HPP