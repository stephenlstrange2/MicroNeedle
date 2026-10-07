#pragma once

#include <Arduino.h>

#include "device_types.h"
#include "tinydecide_backend.h"

static constexpr size_t MICRONEEDLE_INFERENCE_CAPACITY = 4;
static constexpr size_t MICRONEEDLE_INFERENCE_CANDIDATES = MICRONEEDLE_MAX_DEVICES + 1;

enum class InferenceKind : uint8_t { LED_INTENT, DEVICE_ROUTE };

struct InferenceJob {
  uint32_t id = 0;
  InferenceKind kind = InferenceKind::LED_INTENT;
  char prompt[769] = {};
  uint8_t candidateCount = 0;
  char candidateNames[MICRONEEDLE_INFERENCE_CANDIDATES][MICRONEEDLE_ALIAS_SIZE] = {};
  char candidateAliases[MICRONEEDLE_INFERENCE_CANDIDATES][MICRONEEDLE_ALIAS_SIZE] = {};
};

struct InferenceResult {
  uint32_t id = 0;
  InferenceKind kind = InferenceKind::LED_INTENT;
  TinyDecideResult led{};
  DeviceRouteResult device{};
  uint8_t candidateCount = 0;
  char candidateAliases[MICRONEEDLE_INFERENCE_CANDIDATES][MICRONEEDLE_ALIAS_SIZE] = {};
};

class InferenceWorker {
 public:
  explicit InferenceWorker(TinyDecideBackend &backend) : backend_(backend) {}
  bool begin();
  bool submit(const InferenceJob &job);
  bool takeResult(InferenceResult &result);

 private:
  TinyDecideBackend &backend_;
  QueueHandle_t jobs_ = nullptr;
  QueueHandle_t results_ = nullptr;
  TaskHandle_t task_ = nullptr;

  static void taskEntry(void *context);
  void run();
};
