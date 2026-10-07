#pragma once

#include "device_types.h"

class DeviceDriver {
 public:
  virtual ~DeviceDriver() = default;
  virtual bool begin(DeviceBinding &binding) = 0;
  virtual DeviceOperationResult set(DeviceBinding &binding, bool on) = 0;
  virtual DeviceOperationResult setRgb(DeviceBinding &binding, uint8_t r, uint8_t g,
                                       uint8_t b, uint8_t brightness);
  virtual DeviceOperationResult setPattern(DeviceBinding &binding, LedPattern pattern);
  virtual DeviceOperationResult get(DeviceBinding &binding) = 0;
  virtual void tick(DeviceBinding &binding) {}
  virtual void safeStop(DeviceBinding &binding) = 0;
};

class GpioOutputDriver final : public DeviceDriver {
 public:
  bool begin(DeviceBinding &binding) override;
  DeviceOperationResult set(DeviceBinding &binding, bool on) override;
  DeviceOperationResult get(DeviceBinding &binding) override;
  void safeStop(DeviceBinding &binding) override;
};

class Ws2812RgbDriver final : public DeviceDriver {
 public:
  bool begin(DeviceBinding &binding) override;
  DeviceOperationResult set(DeviceBinding &binding, bool on) override;
  DeviceOperationResult setRgb(DeviceBinding &binding, uint8_t r, uint8_t g,
                               uint8_t b, uint8_t brightness) override;
  DeviceOperationResult setPattern(DeviceBinding &binding, LedPattern pattern) override;
  DeviceOperationResult get(DeviceBinding &binding) override;
  void tick(DeviceBinding &binding) override;
  void safeStop(DeviceBinding &binding) override;

 private:
  void render(DeviceBinding &binding, uint8_t scale = 255);
  uint8_t r_ = 0;
  uint8_t g_ = 0;
  uint8_t b_ = 0;
  uint8_t brightness_ = 80;
  LedPattern pattern_ = LedPattern::SOLID;
  uint32_t animationStartedAt_ = 0;
  uint32_t lastFrameAt_ = 0;
};

DeviceDriver *driverFor(DriverType type);
