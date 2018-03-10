#pragma once

#include "common/definitions.h"
#include "tensors/backend.h"
#include "tensors/tensor.h"


namespace marian {
	namespace gpu {
		// output is a vector of size values.size. Output[i] is lower_bound of values[i] in data
		std::vector<int> lower_bounds(int* data,
									  std::vector<int> values,
									  int size,
									  DeviceId device);

		int buildSparse(Tensor t, float* data, int* indices);

		void scatterAdd(Tensor t, float* data, int *indices, int size, int offset);
	}
}