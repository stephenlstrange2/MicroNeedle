#pragma once

#include <Arduino.h>

struct PinCapability {
  uint8_t pin;
  bool inputAllowed;
  bool outputAllowed;
  bool pwmAllowed;
  bool reserved;
  const char *reason;
};

// Conservative profile: set KM_RGB_PIN to match the exact board revision.
// Pins 38 and 48 are both reserved because DevKitC revisions use either one
// for the onboard addressable LED.
class BoardProfile {
 public:
  static const char *name();
  static PinCapability capability(uint8_t pin);
  static bool canBindOutput(uint8_t pin, const char **reason = nullptr);
  static bool isSystemClaimed(uint8_t pin);
};
