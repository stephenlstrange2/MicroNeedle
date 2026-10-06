#pragma once

#include "device_types.h"

class DeviceDriver {
 public:
  virtual ~DeviceDriver() = default;
  virtual bool begin(DeviceBinding &binding) = 0;
  virtual DeviceOperationResult set(DeviceBinding &binding, bool on) = 0;
  virtual DeviceOperationResult get(DeviceBinding &binding) = 0;
  virtual void safeStop(DeviceBinding &binding) = 0;
};

class GpioOutputDriver final : public DeviceDriver {
 public:
  bool begin(DeviceBinding &binding) override;
  DeviceOperationResult set(DeviceBinding &binding, bool on) override;
  DeviceOperationResult get(DeviceBinding &binding) override;
  void safeStop(DeviceBinding &binding) override;
};

DeviceDriver *driverFor(DriverType type);
