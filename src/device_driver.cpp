#include "device_driver.h"

namespace {
GpioOutputDriver gpioOutputDriver;

void writeBinding(const DeviceBinding &binding, bool on) {
  const uint8_t level = (on == binding.activeHigh) ? HIGH : LOW;
  digitalWrite(binding.pin, level);
}
}  // namespace

bool GpioOutputDriver::begin(DeviceBinding &binding) {
  // Establish the safe electrical level before enabling output mode.
  writeBinding(binding, binding.startupOn);
  pinMode(binding.pin, OUTPUT);
  writeBinding(binding, binding.startupOn);
  binding.currentOn = binding.startupOn;
  return true;
}

DeviceOperationResult GpioOutputDriver::set(DeviceBinding &binding, bool on) {
  writeBinding(binding, on);
  binding.currentOn = on;
  return {true, nullptr, on};
}

DeviceOperationResult GpioOutputDriver::get(DeviceBinding &binding) {
  return {true, nullptr, binding.currentOn};
}

void GpioOutputDriver::safeStop(DeviceBinding &binding) {
  writeBinding(binding, false);
  binding.currentOn = false;
}

DeviceDriver *driverFor(DriverType type) {
  switch (type) {
    case DriverType::GPIO_OUTPUT: return &gpioOutputDriver;
  }
  return nullptr;
}
