#include "device_driver.h"

#include <Adafruit_NeoPixel.h>
#include <math.h>

#ifndef KM_RGB_PIN
#define KM_RGB_PIN 48
#endif

namespace {
GpioOutputDriver gpioOutputDriver;
Ws2812RgbDriver ws2812RgbDriver;
Adafruit_NeoPixel systemPixel(1, KM_RGB_PIN, NEO_GRB + NEO_KHZ800);

void writeBinding(const DeviceBinding &binding, bool on) {
  const uint8_t level = (on == binding.activeHigh) ? HIGH : LOW;
  digitalWrite(binding.pin, level);
}

uint32_t wheel(uint8_t position) {
  position = 255 - position;
  if (position < 85) return systemPixel.Color(255 - position * 3, 0, position * 3);
  if (position < 170) {
    position -= 85;
    return systemPixel.Color(0, position * 3, 255 - position * 3);
  }
  position -= 170;
  return systemPixel.Color(position * 3, 255 - position * 3, 0);
}
}  // namespace

DeviceOperationResult DeviceDriver::setRgb(DeviceBinding &, uint8_t, uint8_t, uint8_t, uint8_t) {
  return {false, "operation_not_supported"};
}

DeviceOperationResult DeviceDriver::setPattern(DeviceBinding &, LedPattern) {
  return {false, "operation_not_supported"};
}

bool GpioOutputDriver::begin(DeviceBinding &binding) {
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

bool Ws2812RgbDriver::begin(DeviceBinding &binding) {
  systemPixel.begin();
  systemPixel.clear();
  systemPixel.show();
  binding.currentOn = false;
  return true;
}

void Ws2812RgbDriver::render(DeviceBinding &binding, uint8_t scale) {
  systemPixel.setBrightness(static_cast<uint8_t>((static_cast<uint16_t>(brightness_) * scale) / 255));
  systemPixel.setPixelColor(0, binding.currentOn ? systemPixel.Color(r_, g_, b_) : 0);
  systemPixel.show();
}

DeviceOperationResult Ws2812RgbDriver::set(DeviceBinding &binding, bool on) {
  if (on && r_ == 0 && g_ == 0 && b_ == 0) r_ = g_ = b_ = 255;
  binding.currentOn = on;
  if (!on) pattern_ = LedPattern::SOLID;
  render(binding);
  return get(binding);
}

DeviceOperationResult Ws2812RgbDriver::setRgb(DeviceBinding &binding, uint8_t r, uint8_t g,
                                               uint8_t b, uint8_t brightness) {
  r_ = r;
  g_ = g;
  b_ = b;
  brightness_ = brightness;
  pattern_ = LedPattern::SOLID;
  binding.currentOn = brightness > 0 && (r || g || b);
  render(binding);
  return get(binding);
}

DeviceOperationResult Ws2812RgbDriver::setPattern(DeviceBinding &binding, LedPattern pattern) {
  if (r_ == 0 && g_ == 0 && b_ == 0) r_ = g_ = b_ = 255;
  pattern_ = pattern;
  binding.currentOn = true;
  animationStartedAt_ = millis();
  lastFrameAt_ = 0;
  render(binding);
  return get(binding);
}

DeviceOperationResult Ws2812RgbDriver::get(DeviceBinding &binding) {
  DeviceOperationResult result{true, nullptr, binding.currentOn};
  result.r = r_;
  result.g = g_;
  result.b = b_;
  result.brightness = brightness_;
  result.pattern = pattern_;
  return result;
}

void Ws2812RgbDriver::tick(DeviceBinding &binding) {
  if (!binding.currentOn || pattern_ == LedPattern::SOLID) return;
  const uint32_t now = millis();
  if (now - lastFrameAt_ < 25) return;
  lastFrameAt_ = now;
  const uint32_t elapsed = now - animationStartedAt_;
  if (pattern_ == LedPattern::BLINK) {
    const bool visible = ((elapsed / 500) % 2) == 0;
    systemPixel.setBrightness(brightness_);
    systemPixel.setPixelColor(0, visible ? systemPixel.Color(r_, g_, b_) : 0);
    systemPixel.show();
  } else if (pattern_ == LedPattern::PULSE) {
    const float phase = static_cast<float>(elapsed % 2000) * (2.0f * PI / 2000.0f);
    const uint8_t scale = static_cast<uint8_t>((sinf(phase - PI / 2.0f) + 1.0f) * 127.5f);
    render(binding, scale);
  } else if (pattern_ == LedPattern::RAINBOW) {
    systemPixel.setBrightness(brightness_);
    systemPixel.setPixelColor(0, wheel(static_cast<uint8_t>((elapsed / 10) & 0xFF)));
    systemPixel.show();
  }
}

void Ws2812RgbDriver::safeStop(DeviceBinding &binding) {
  pattern_ = LedPattern::SOLID;
  binding.currentOn = false;
  render(binding);
}

DeviceDriver *driverFor(DriverType type) {
  switch (type) {
    case DriverType::GPIO_OUTPUT: return &gpioOutputDriver;
    case DriverType::WS2812_RGB: return &ws2812RgbDriver;
  }
  return nullptr;
}
