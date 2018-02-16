#include <cuda.h>
#include <curand.h>
#include <stdio.h>
#include <stdlib.h>

#include "backend/dispatch.h"
#include "graph/backend_gpu.h"

#define CUDA_CALL(x)                                  \
  do {                                                \
    if((x) != cudaSuccess) {                          \
      printf("Error at %s:%d\n", __FILE__, __LINE__); \
      exit(1);                                        \
    }                                                 \
  } while(0)

#define CURAND_CALL(x)                                \
  do {                                                \
    if((x) != CURAND_STATUS_SUCCESS) {                \
      printf("Error at %s:%d\n", __FILE__, __LINE__); \
      exit(1);                                        \
    }                                                 \
  } while(0)


namespace marian {
  namespace gpu {
    
    __global__ void gScale(float* data, int n, float p) {
      int index = threadIdx.x + blockIdx.x * blockDim.x;
    
      while(index < n) {
        data[index] = (data[index] < p) / p;
        index += gridDim.x * blockDim.x;
      }
    }
    
    void Dropout(Ptr<Backend> backend, Tensor tensor, float p) {
      curandGenerator_t gen = std::static_pointer_cast<BackendGPU>(backend)->getCurandGenerator();
      int n = tensor->size();
      CURAND_CALL(curandGenerateUniform(gen, tensor->data(), n));
    
      int numThreads = std::min(n, 512);
      int numBlocks = n / numThreads + (n % numThreads != 0);
    
      gScale<<<numBlocks, numThreads>>>(tensor->data(), n, 1.f - p);
    }
    
  }
}
