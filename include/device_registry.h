#pragma once

#include <Arduino.h>

#include "board_profile.h"
#include "device_driver.h"
#include "device_types.h"

class DeviceRegistry {
 public:
  bool begin();

  DeviceOperationResult bindGpioOutput(const char *displayName, uint8_t pin,
                                       bool activeHigh, bool startupOn,
                                       DeviceBinding **created = nullptr);
  DeviceOperationResult unbind(const char *alias);
  DeviceOperationResult rename(const char *alias, const char *newDisplayName);
  DeviceOperationResult set(const char *alias, bool on);
  DeviceOperationResult setRgb(const char *alias, uint8_t r, uint8_t g, uint8_t b,
                               uint8_t brightness);
  DeviceOperationResult setPattern(const char *alias, LedPattern pattern);
  DeviceOperationResult get(const char *alias);
  void tick();

  DeviceBinding *find(const char *alias);
  const DeviceBinding *find(const char *alias) const;
  DeviceBinding *bindingAt(size_t slot);
  const DeviceBinding *bindingAt(size_t slot) const;
  size_t count() const;
  size_t visibleCount() const;
  static constexpr size_t bindingSlots() { return MICRONEEDLE_MAX_DEVICES + 1; }
  bool pinClaimed(uint8_t pin) const;
  bool save();
  void clear();

  static bool normalizeAlias(const char *input, char *output, size_t outputSize);

 private:
  DeviceBinding bindings_[MICRONEEDLE_MAX_DEVICES]{};
  DeviceBinding systemLed_{};
  uint32_t nextId_ = 1;

  bool initializeSystemLed();
  bool isSystemDevice(const DeviceBinding *binding) const { return binding == &systemLed_; }
  bool load();
  int freeSlot() const;
};
