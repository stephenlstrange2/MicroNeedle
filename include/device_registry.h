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
  DeviceOperationResult get(const char *alias);

  DeviceBinding *find(const char *alias);
  const DeviceBinding *find(const char *alias) const;
  DeviceBinding *bindingAt(size_t slot);
  const DeviceBinding *bindingAt(size_t slot) const;
  size_t count() const;
  bool pinClaimed(uint8_t pin) const;
  bool save();
  void clear();

  static bool normalizeAlias(const char *input, char *output, size_t outputSize);

 private:
  DeviceBinding bindings_[MICRONEEDLE_MAX_DEVICES]{};
  uint32_t nextId_ = 1;

  bool load();
  int freeSlot() const;
};
