#include "board_profile.h"

#ifndef KM_RGB_PIN
#define KM_RGB_PIN 48
#endif

const char *BoardProfile::name() {
  return "esp32-s3-devkitc-1-n16r8";
}

bool BoardProfile::isSystemClaimed(uint8_t pin) {
  return pin == KM_RGB_PIN || pin == 38 || pin == 48;
}

PinCapability BoardProfile::capability(uint8_t pin) {
  PinCapability result{pin, false, false, false, true, "pin is outside the supported board profile"};
  if (pin > 48) return result;

  // Conservative N16R8 profile. Do not expose boot-strapping, native USB,
  // UART console, octal flash/PSRAM, or the onboard RGB status pin.
  if (pin == 0 || pin == 3 || pin == 45 || pin == 46) {
    result.reason = "boot-strapping or restricted pin";
    return result;
  }
  if (pin == 19 || pin == 20) {
    result.reason = "native USB pin";
    return result;
  }
  if ((pin >= 26 && pin <= 37)) {
    result.reason = "flash/OPI PSRAM pin";
    return result;
  }
  if (pin >= 39 && pin <= 42) {
    result.reason = "default JTAG/debug pin";
    return result;
  }
  if (pin == 43 || pin == 44) {
    result.reason = "serial console pin";
    return result;
  }
  if (isSystemClaimed(pin)) {
    result.reason = "claimed by the onboard WS2812B status driver";
    return result;
  }

  const bool safe = (pin >= 1 && pin <= 18) || pin == 21 ||
                    (pin >= 38 && pin <= 42) || pin == 47;
  if (!safe) return result;

  result.inputAllowed = true;
  result.outputAllowed = true;
  result.pwmAllowed = true;
  result.reserved = false;
  result.reason = nullptr;
  return result;
}

bool BoardProfile::canBindOutput(uint8_t pin, const char **reason) {
  PinCapability cap = capability(pin);
  if (reason) *reason = cap.reason;
  return !cap.reserved && cap.outputAllowed;
}
