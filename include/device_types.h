#pragma once

#include <Arduino.h>

static constexpr size_t MICRONEEDLE_MAX_DEVICES = 16;
static constexpr size_t MICRONEEDLE_ALIAS_SIZE = 32;

enum class DriverType : uint8_t {
  GPIO_OUTPUT = 1,
  WS2812_RGB = 2,
};

enum DeviceCapability : uint16_t {
  CAP_NONE = 0,
  CAP_ON = 1 << 0,
  CAP_OFF = 1 << 1,
  CAP_STATE = 1 << 2,
  CAP_COLOR = 1 << 3,
  CAP_BRIGHTNESS = 1 << 4,
  CAP_PATTERN = 1 << 5,
};

struct DeviceBinding {
  uint32_t id = 0;
  char alias[MICRONEEDLE_ALIAS_SIZE] = {};
  char displayName[MICRONEEDLE_ALIAS_SIZE] = {};
  DriverType driver = DriverType::GPIO_OUTPUT;
  uint8_t pin = 0;
  bool activeHigh = true;
  bool startupOn = false;
  bool currentOn = false;
  uint16_t capabilities = CAP_NONE;
  bool occupied = false;
};

enum class LedPattern : uint8_t { SOLID = 0, BLINK = 1, PULSE = 2, RAINBOW = 3 };

struct DeviceOperationResult {
  bool success = false;
  const char *error = nullptr;
  bool state = false;
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t brightness = 0;
  LedPattern pattern = LedPattern::SOLID;
};
