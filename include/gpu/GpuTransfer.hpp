/**
 * @file include/gpu/GpuTransfer.hpp
 * @brief Small CUDA RAII helpers used by the GPU transport experiments.
 */
#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace polygraph {

inline void cuda_check(const cudaError_t error, const char* where) {
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("CUDA error at ") + where + ": " + cudaGetErrorString(error));
    }
}

/** CUDA's active device is thread-local; worker threads must select the projection's device. */
class CudaDeviceGuard {
  public:
    explicit CudaDeviceGuard(const int target_device) {
        cuda_check(cudaGetDevice(&previous_device_), "cudaGetDevice");
        if (previous_device_ != target_device) {
            cuda_check(cudaSetDevice(target_device), "cudaSetDevice");
            changed_ = true;
        }
    }

    CudaDeviceGuard(const CudaDeviceGuard&) = delete;
    CudaDeviceGuard& operator=(const CudaDeviceGuard&) = delete;

    ~CudaDeviceGuard() {
        if (changed_) {
            (void)cudaSetDevice(previous_device_);
        }
    }

  private:
    int previous_device_{0};
    bool changed_{false};
};

template <typename T> class PinnedHostBuffer {
  public:
    PinnedHostBuffer() = default;

    explicit PinnedHostBuffer(const std::size_t size) { resize(size); }

    PinnedHostBuffer(const PinnedHostBuffer&) = delete;
    PinnedHostBuffer& operator=(const PinnedHostBuffer&) = delete;

    PinnedHostBuffer(PinnedHostBuffer&& other) noexcept { swap(other); }
    PinnedHostBuffer& operator=(PinnedHostBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            swap(other);
        }
        return *this;
    }

    ~PinnedHostBuffer() { reset(); }

    void resize(const std::size_t size) {
        if (size == size_) {
            return;
        }
        reset();
        if (size == 0) {
            return;
        }
        void* raw = nullptr;
        cuda_check(cudaHostAlloc(&raw, size * sizeof(T), cudaHostAllocPortable), "cudaHostAlloc");
        data_ = static_cast<T*>(raw);
        size_ = size;
    }

    void reset() noexcept {
        if (data_ != nullptr) {
            (void)cudaFreeHost(data_);
            data_ = nullptr;
            size_ = 0;
        }
    }

    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] T& operator[](const std::size_t index) noexcept { return data_[index]; }
    [[nodiscard]] const T& operator[](const std::size_t index) const noexcept { return data_[index]; }

  private:
    void swap(PinnedHostBuffer& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
    }

    T* data_{nullptr};
    std::size_t size_{0};
};

/** Temporarily page-lock an existing contiguous host allocation. */
class ScopedHostRegistration {
  public:
    ScopedHostRegistration() = default;

    ScopedHostRegistration(void* ptr, const std::size_t bytes) : ptr_(ptr), bytes_(bytes) {
        if (ptr_ == nullptr || bytes_ == 0) {
            return;
        }
        const auto status = cudaHostRegister(ptr_, bytes_, cudaHostRegisterPortable);
        if (status == cudaSuccess) {
            registered_ = true;
        } else {
            // Registration is an optimization. Clear the sticky CUDA error and
            // gracefully fall back to pageable memory when the OS pin limit or
            // platform does not permit it.
            (void)cudaGetLastError();
        }
    }

    ScopedHostRegistration(const ScopedHostRegistration&) = delete;
    ScopedHostRegistration& operator=(const ScopedHostRegistration&) = delete;

    ~ScopedHostRegistration() {
        if (registered_) {
            (void)cudaHostUnregister(ptr_);
        }
    }

    [[nodiscard]] bool registered() const noexcept { return registered_; }

  private:
    void* ptr_{nullptr};
    std::size_t bytes_{0};
    bool registered_{false};
};

class CudaStream {
  public:
    CudaStream() { cuda_check(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "cudaStreamCreateWithFlags"); }
    CudaStream(const CudaStream&) = delete;
    CudaStream& operator=(const CudaStream&) = delete;
    ~CudaStream() {
        if (stream_ != nullptr) {
            (void)cudaStreamDestroy(stream_);
        }
    }
    [[nodiscard]] cudaStream_t get() const noexcept { return stream_; }

  private:
    cudaStream_t stream_{nullptr};
};

class CudaEvent {
  public:
    explicit CudaEvent(const unsigned int flags = cudaEventDisableTiming) {
        cuda_check(cudaEventCreateWithFlags(&event_, flags), "cudaEventCreateWithFlags");
    }
    CudaEvent(const CudaEvent&) = delete;
    CudaEvent& operator=(const CudaEvent&) = delete;
    ~CudaEvent() {
        if (event_ != nullptr) {
            (void)cudaEventDestroy(event_);
        }
    }
    [[nodiscard]] cudaEvent_t get() const noexcept { return event_; }

  private:
    cudaEvent_t event_{nullptr};
};

/** Compact O(1) deduplication for dense vertex IDs. */
class DenseVisitBitmap {
  public:
    explicit DenseVisitBitmap(const std::size_t vertex_count) : words_((vertex_count + 63U) / 64U, 0U) {}

    [[nodiscard]] bool test_and_set(const std::size_t dense_id) noexcept {
        const auto word = dense_id >> 6U;
        const auto bit = dense_id & 63U;
        const std::uint64_t mask = std::uint64_t{1} << bit;
        const bool already_set = (words_[word] & mask) != 0U;
        words_[word] |= mask;
        return already_set;
    }

    [[nodiscard]] std::size_t bytes() const noexcept { return words_.size() * sizeof(std::uint64_t); }

  private:
    std::vector<std::uint64_t> words_;
};

} // namespace polygraph
