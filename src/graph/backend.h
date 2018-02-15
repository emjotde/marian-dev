#pragma once

#include "common/definitions.h"

namespace marian {

class Backend {
protected:
  DeviceId deviceId_;
  size_t seed_;
  
public:
  Backend(DeviceId deviceId, size_t seed)
    : deviceId_(deviceId), seed_(seed) {}
  
  virtual DeviceId getDevice() { return deviceId_; };
};

}
