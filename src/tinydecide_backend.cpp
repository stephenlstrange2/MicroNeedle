#include "tinydecide_backend.h"

#include <cstdlib>
#include <cstring>

namespace {
constexpr float ACTION_THRESHOLD = 0.50f;
constexpr float COLOR_THRESHOLD = 0.30f;

const char *const ACTIONS[] = {
    "set the LED color",
    "turn the LED off",
    "unsupported request",
};
const char *const COLORS[] = {
    "red", "green", "blue", "orange", "yellow",
    "purple", "pink", "white", "no color specified",
};

const td::Question QUESTIONS[] = {
    {td::CHOICE, "Which action should handle this message?", ACTIONS, 3},
    {td::CHOICE, "Which LED color did the user request?", COLORS, 9},
    {td::SPAN, "Extract the requested brightness number from 0 to 255."},
};

static td::Answer answers[3];

void setColor(int pick, TinyDecideResult &result) {
  switch (pick) {
    case 0: result.r = 255; result.g = 0; result.b = 0; break;
    case 1: result.r = 0; result.g = 255; result.b = 0; break;
    case 2: result.r = 0; result.g = 0; result.b = 255; break;
    case 3: result.r = 255; result.g = 80; result.b = 0; break;
    case 4: result.r = 255; result.g = 180; result.b = 0; break;
    case 5: result.r = 160; result.g = 0; result.b = 255; break;
    case 6: result.r = 255; result.g = 20; result.b = 100; break;
    case 7: result.r = 255; result.g = 255; result.b = 255; break;
    default: break;
  }
}

bool parseBrightnessSpan(const String &prompt, const td::Answer &answer, uint8_t &brightness) {
  if (answer.start < 0 || answer.end <= answer.start || answer.p_present < 0.50f ||
      static_cast<size_t>(answer.end) > prompt.length()) {
    return false;
  }
  String span = prompt.substring(answer.start, answer.end);
  const char *cursor = span.c_str();
  while (*cursor && !isDigit(static_cast<unsigned char>(*cursor))) ++cursor;
  if (!*cursor) return false;
  char *end = nullptr;
  long value = strtol(cursor, &end, 10);
  if (end == cursor || value < 0 || value > 255) return false;
  brightness = static_cast<uint8_t>(value);
  return true;
}
}  // namespace

bool TinyDecideBackend::begin() {
  mutex_ = xSemaphoreCreateMutex();
  if (!mutex_) return false;
  ready_ = td::initPartition("tinydecide");
  return ready_;
}

TinyDecideResult TinyDecideBackend::interpret(const String &prompt) {
  TinyDecideResult result;
  if (!ready_) {
    result.error = "model_not_ready";
    return result;
  }
  if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
    result.error = "model_busy";
    return result;
  }

  td::Info info{};
  const td::Status status = td::answer(prompt.c_str(), QUESTIONS, 3, answers, &info);
  result.elapsedMs = info.ms;
  result.tokens = info.tokens;
  result.truncated = info.truncated;
  if (status != td::OK) {
    result.error = td::statusText(status);
    xSemaphoreGive(mutex_);
    return result;
  }

  const int action = answers[0].pick;
  result.confidence = answers[0].probs[action];
  result.decisionConfidence = answers[0].confidence;
  if (action == 2 || result.confidence < ACTION_THRESHOLD) {
    result.action = TinyDecideResult::UNSUPPORTED;
    xSemaphoreGive(mutex_);
    return result;
  }
  if (action == 1) {
    result.action = TinyDecideResult::LED_OFF;
    xSemaphoreGive(mutex_);
    return result;
  }

  const int color = answers[1].pick;
  result.colorConfidence = answers[1].probs[color];
  if (color < 0 || color >= 8 || result.colorConfidence < COLOR_THRESHOLD) {
    result.action = TinyDecideResult::UNSUPPORTED;
    result.error = "color_not_grounded";
    xSemaphoreGive(mutex_);
    return result;
  }

  setColor(color, result);
  parseBrightnessSpan(prompt, answers[2], result.brightness);
  result.action = TinyDecideResult::SET_LED;
  xSemaphoreGive(mutex_);
  return result;
}
