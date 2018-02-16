#include "tensors/gpu/algorithm.h"

#include "kernels/cuda_helpers.h"

namespace marian {
  namespace gpu {
    void copy(Ptr<Backend> backend, const float* begin, const float* end, float* dest) {
      CUDA_CHECK(cudaSetDevice(backend->getDevice().no));
      CudaCopy(begin, end, dest);
      CUDA_CHECK(cudaStreamSynchronize(0));
    }

    __global__ void gFill(float *d_in, int size, float val) {
      for(int bid = 0; bid < size; bid += blockDim.x * gridDim.x) {
        int index = bid + threadIdx.x + blockDim.x * blockIdx.x;
        if(index < size) {
          d_in[index] = val;
        }
      }
    }

    void fill(Ptr<Backend> backend, float* begin, float* end, float value) {
      CUDA_CHECK(cudaSetDevice(backend->getDevice().no));
      int size = end - begin;
      int threads = std::min(512, size);
      int blocks = (size / threads) + (size % threads != 0);
      gFill<<<blocks, threads>>>(begin, size, value);
      CUDA_CHECK(cudaStreamSynchronize(0));
    }
  }
}
