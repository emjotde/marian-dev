/* All or part of this file was contributed by Intel under license:
 *   Copyright (C) 2017-2018 Intel Corporation
 *   SPDX-License-Identifier: MIT
 */

#include <limits>

#include "data/types.h"
#include "tensors/tensor.h"
#include "translator/helpers.h"

namespace marian {

namespace cpu {

void SetColumn(Tensor in_, size_t col, float value) {
  int nRows = in_->shape().elements() / in_->shape()[-1];
  int nColumns = in_->shape()[-1];

  float* in = in_->data();
  for(int rowNumber = 0; rowNumber < nRows; ++rowNumber) {
    int index = col + rowNumber * nColumns;
    in[index] = value;
  }
}

void suppressWord(Expr probs, Word id) {
  SetColumn(probs->val(), id, std::numeric_limits<float>::lowest());
}
}  // namespace cpu

void suppressWord(Expr probs, Word id) {
  if(probs->val()->getBackend()->getDeviceId().type == DeviceType::cpu) {
    cpu::suppressWord(probs, id);
  }
#ifdef CUDA_FOUND
  else {
    gpu::suppressWord(probs, id);
  }
#endif
}
}  // namespace marian
