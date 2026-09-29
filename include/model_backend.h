#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// A model backend interprets text but never controls hardware directly.
// Implementations must emit a versioned tool request that the firmware
// command validator accepts before any device action is performed.
class ModelBackend {
 public:
  virtual ~ModelBackend() = default;

  virtual bool begin() = 0;

  // Writes a tool request into output. Return false for unsupported or
  // low-confidence input. The caller remains responsible for validation.
  virtual bool interpret(const String &prompt, JsonDocument &output) = 0;

  virtual const char *name() const = 0;
  virtual float confidence() const = 0;
};

// Reserved for the first ESP32-S3 model implementation. The initial firmware
// continues to use its deterministic parser until a trained model artifact
// and a measured embedded runtime are added.
class OnDeviceIntentBackend final : public ModelBackend {
 public:
  bool begin() override { return false; }
  bool interpret(const String &, JsonDocument &) override { return false; }
  const char *name() const override { return "on-device-intent-unavailable"; }
  float confidence() const override { return 0.0f; }
};
