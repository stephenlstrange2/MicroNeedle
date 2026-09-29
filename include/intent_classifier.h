#pragma once

#include <Arduino.h>
#include <math.h>
#include "../model/intent_model.h"

struct IntentPrediction {
  const char *label;
  float confidence;
};

class IntentClassifier {
 public:
  IntentPrediction predict(String text) const {
    text.trim();
    text.toLowerCase();
    text = " " + text + " ";
    float scores[MICRONEEDLE_MODEL_CLASSES];
    for (int c = 0; c < MICRONEEDLE_MODEL_CLASSES; ++c) {
      scores[c] = microneedle_model_bias[c];
    }
    for (int n = 2; n <= 5; ++n) {
      for (size_t i = 0; i + n <= text.length(); ++i) {
        uint32_t hash = 2166136261u;
        for (int j = 0; j < n; ++j) {
          const uint8_t *bytes = reinterpret_cast<const uint8_t *>(text.c_str() + i + j);
          hash ^= *bytes;
          hash *= 16777619u;
        }
        const uint16_t feature = hash % MICRONEEDLE_MODEL_FEATURES;
        for (int c = 0; c < MICRONEEDLE_MODEL_CLASSES; ++c) {
          scores[c] += microneedle_model_weights[c * MICRONEEDLE_MODEL_FEATURES + feature];
        }
      }
    }

    int best = 0;
    for (int c = 1; c < MICRONEEDLE_MODEL_CLASSES; ++c) {
      if (scores[c] > scores[best]) best = c;
    }
    float denominator = 0.0f;
    float maximum = scores[best];
    for (int c = 0; c < MICRONEEDLE_MODEL_CLASSES; ++c) denominator += expf(scores[c] - maximum);
    return {microneedle_model_labels[best], 1.0f / denominator};
  }
};
