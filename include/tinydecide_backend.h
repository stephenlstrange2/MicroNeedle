#pragma once

#include <Arduino.h>
#include <tinydecide.h>

struct DeviceRouteResult {
  enum Action : uint8_t { SET_ON, SET_OFF, GET_STATE, UNSUPPORTED, ERROR } action = ERROR;
  int targetIndex = -1;
  float confidence = 0.0f;
  float targetConfidence = 0.0f;
  uint32_t elapsedMs = 0;
  int tokens = 0;
  bool truncated = false;
  const char *error = nullptr;
};

struct TinyDecideResult {
  enum Action : uint8_t { SET_LED, LED_OFF, UNSUPPORTED, ERROR } action = ERROR;
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t brightness = 80;
  float confidence = 0.0f;
  float decisionConfidence = 0.0f;
  float colorConfidence = 0.0f;
  uint32_t elapsedMs = 0;
  int tokens = 0;
  bool truncated = false;
  const char *error = nullptr;
};

class TinyDecideBackend {
 public:
  bool begin();
  bool ready() const { return ready_; }
  TinyDecideResult interpret(const String &prompt);
  DeviceRouteResult routeDevice(const String &prompt, const char *const *deviceNames,
                                size_t deviceCount);
  const char *name() const { return "tinydecide-10.4m-q4"; }

 private:
  bool ready_ = false;
  SemaphoreHandle_t mutex_ = nullptr;
};
