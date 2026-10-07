#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "model_backend.h"
#include "intent_classifier.h"
#include "tinydecide_backend.h"
#include "device_registry.h"
#include "task_executor.h"
#include "confirmation_manager.h"
#include "command_validator.h"
#include "tool_registry.h"
#include "ingress_queue.h"
#include "response_sink.h"
#include "inference_worker.h"

#ifndef KM_RGB_PIN
#define KM_RGB_PIN 48
#endif

#ifndef MICRONEEDLE_PROTOCOL_VERSION
#define MICRONEEDLE_PROTOCOL_VERSION 1
#endif

static constexpr size_t MICRONEEDLE_MAX_INPUT = 768;
AsyncWebServer webServer(80);
AsyncEventSource taskEvents("/events");
IngressQueue ingressQueue;
SerialResponseSink serialResponseSink;
IntentClassifier intentClassifier;
TinyDecideBackend tinyDecide;
InferenceWorker inferenceWorker(tinyDecide);
DeviceRegistry deviceRegistry;
TaskExecutor taskExecutor(deviceRegistry);
ConfirmationManager confirmations;

struct RgbValue {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct PendingInference {
  bool active = false;
  uint32_t jobId = 0;
  IngressRequest ingress{};
  char requestId[48] = {};
  char prompt[MICRONEEDLE_INGRESS_PAYLOAD_SIZE] = {};
};

PendingInference pendingInference[MICRONEEDLE_INFERENCE_CAPACITY]{};
IngressRequest *activeIngress = nullptr;
uint32_t nextInferenceId = 1;
bool inferenceReady = false;

const char *driverName(DriverType driver) {
  return driver == DriverType::WS2812_RGB ? "ws2812.rgb" : "gpio.output";
}

const char *patternName(LedPattern pattern) {
  switch (pattern) {
    case LedPattern::SOLID: return "solid";
    case LedPattern::BLINK: return "blink";
    case LedPattern::PULSE: return "pulse";
    case LedPattern::RAINBOW: return "rainbow";
  }
  return "unknown";
}

void sendError(const char *id, const char *code, const char *message) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = false;
  response["error"]["code"] = code;
  response["error"]["message"] = message;
  emitResponse(response, 422);
}

void sendSuccess(const char *id) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = true;
  emitResponse(response);
}

void sendTaskAccepted(const char *id, uint32_t taskId, const char *tool,
                      const char *backend = nullptr, float confidence = -1.0f) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = true;
  JsonObject task = response["task"].to<JsonObject>();
  task["id"] = taskId;
  task["tool"] = tool;
  task["status"] = "queued";
  if (backend) {
    JsonObject routing = response["routing"].to<JsonObject>();
    routing["backend"] = backend;
    routing["mode"] = strstr(backend, "tinydecide") ? "model" : "deterministic";
    if (confidence >= 0.0f) routing["confidence"] = confidence;
  }
  emitResponse(response);
}

void sendConfirmation(const char *id, uint32_t confirmationId, const TaskRequest &proposal) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = false;
  response["requires_confirmation"] = true;
  response["confirmation_id"] = confirmationId;
  response["confirmation_prompt"] = String("confirm ") + confirmationId;
  response["expires_in_seconds"] = 30;
  response["confirmation_session"] = currentSessionId();
  JsonObject proposed = response["proposed"].to<JsonObject>();
  proposed["tool"] = TaskExecutor::operationName(proposal.operation);
  JsonObject args = proposed["args"].to<JsonObject>();
  args["alias"] = proposal.alias;
  if (proposal.operation == TaskOperation::DEVICE_BIND) {
    args["driver"] = "gpio.output";
    args["pin"] = proposal.pin;
    args["active_level"] = proposal.activeHigh ? "high" : "low";
    args["startup_state"] = proposal.startupOn ? "on" : "off";
  } else if (proposal.operation == TaskOperation::DEVICE_RENAME) {
    args["new_alias"] = proposal.newAlias;
  }
  emitResponse(response);
}

bool proposeConfirmation(const char *id, const TaskRequest &request) {
  const char *validationError = nullptr;
  if (!CommandValidator::validate(request, deviceRegistry, &validationError)) {
    sendError(id, validationError ? validationError : "validation_failed",
              "Proposed configuration failed centralized validation");
    return false;
  }
  const uint32_t confirmationId = confirmations.propose(request, currentSessionId());
  if (!confirmationId) {
    sendError(id, "confirmation_capacity_full", "Too many pending confirmations");
    return false;
  }
  sendConfirmation(id, confirmationId, request);
  return true;
}

bool submitTask(const char *id, const TaskRequest &request, const char *backend = nullptr,
                float confidence = -1.0f) {
  uint32_t taskId = 0;
  const char *error = nullptr;
  if (!taskExecutor.submit(request, taskId, &error)) {
    sendError(id, error ? error : "task_submit_failed", "Task could not be queued");
    return false;
  }
  sendTaskAccepted(id, taskId, TaskExecutor::operationName(request.operation), backend, confidence);
  return true;
}

void sendDeviceList(const char *id) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = true;
  JsonArray devices = response["result"]["devices"].to<JsonArray>();
  for (size_t i = 0; i < DeviceRegistry::bindingSlots(); ++i) {
    const DeviceBinding *binding = deviceRegistry.bindingAt(i);
    if (!binding || !binding->occupied) continue;
    JsonObject device = devices.add<JsonObject>();
    device["id"] = binding->id;
    device["alias"] = binding->alias;
    device["display_name"] = binding->displayName;
    device["driver"] = driverName(binding->driver);
    device["pin"] = binding->pin;
    device["system"] = binding->driver == DriverType::WS2812_RGB;
    if (binding->driver == DriverType::GPIO_OUTPUT)
      device["active_level"] = binding->activeHigh ? "high" : "low";
    device["state"] = binding->currentOn ? "on" : "off";
  }
  emitResponse(response);
}

void sendDeviceDescription(const char *id, const char *alias) {
  const DeviceBinding *binding = deviceRegistry.find(alias);
  if (!binding) {
    sendError(id, "device_not_found", "No registered device has that alias");
    return;
  }
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = true;
  JsonObject device = response["result"]["device"].to<JsonObject>();
  device["id"] = binding->id;
  device["alias"] = binding->alias;
  device["display_name"] = binding->displayName;
  device["driver"] = driverName(binding->driver);
  device["pin"] = binding->pin;
  device["system"] = binding->driver == DriverType::WS2812_RGB;
  if (binding->driver == DriverType::GPIO_OUTPUT) {
    device["active_level"] = binding->activeHigh ? "high" : "low";
    device["startup_state"] = binding->startupOn ? "on" : "off";
  }
  device["state"] = binding->currentOn ? "on" : "off";
  if (binding->driver == DriverType::WS2812_RGB) {
    DeviceOperationResult led = deviceRegistry.get(binding->alias);
    device["color"]["r"] = led.r;
    device["color"]["g"] = led.g;
    device["color"]["b"] = led.b;
    device["brightness"] = led.brightness;
    device["pattern"] = patternName(led.pattern);
  }
  JsonArray capabilities = device["capabilities"].to<JsonArray>();
  if (binding->capabilities & CAP_ON) capabilities.add("on");
  if (binding->capabilities & CAP_OFF) capabilities.add("off");
  if (binding->capabilities & CAP_STATE) capabilities.add("state");
  if (binding->capabilities & CAP_COLOR) capabilities.add("color");
  if (binding->capabilities & CAP_BRIGHTNESS) capabilities.add("brightness");
  if (binding->capabilities & CAP_PATTERN) capabilities.add("pattern");
  emitResponse(response);
}

void sendTaskRecord(const char *id, const TaskRecord &record, bool event = false) {
  JsonDocument response;
  if (event) response["event"] = String("task.") + TaskExecutor::statusName(record.status);
  else {
    response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
    response["id"] = id ? id : "";
    response["success"] = true;
  }
  JsonObject task = response["task"].to<JsonObject>();
  task["id"] = record.id;
  task["tool"] = TaskExecutor::operationName(record.request.operation);
  task["status"] = TaskExecutor::statusName(record.status);
  task["alias"] = record.request.alias;
  if (record.status == TaskStatus::COMPLETED &&
      (record.request.operation == TaskOperation::DEVICE_GET ||
       record.request.operation == TaskOperation::DEVICE_SET ||
       record.request.operation == TaskOperation::DEVICE_SET_RGB ||
       record.request.operation == TaskOperation::DEVICE_SET_PATTERN)) {
    task["result"]["state"] = record.resultState ? "on" : "off";
    if (record.request.operation == TaskOperation::DEVICE_SET_RGB ||
        record.request.operation == TaskOperation::DEVICE_SET_PATTERN ||
        strcmp(record.request.alias, "status_led") == 0) {
      task["result"]["color"]["r"] = record.resultR;
      task["result"]["color"]["g"] = record.resultG;
      task["result"]["color"]["b"] = record.resultB;
      task["result"]["brightness"] = record.resultBrightness;
      task["result"]["pattern"] = patternName(record.resultPattern);
    }
  }
  if (record.error[0]) task["error"] = record.error;
  if (event) emitEvent(response, &taskEvents);
  else emitResponse(response);
}

void sendPromptResponse(const char *id, const IntentPrediction &prediction,
                        bool success, const char *errorCode = nullptr,
                        const char *errorMessage = nullptr) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = success;
  response["routing"]["backend"] = "ngram-fallback";
  response["routing"]["mode"] = "model";
  response["routing"]["confidence"] = prediction.confidence;
  JsonObject result = response["result"].to<JsonObject>();
  result["intent"] = prediction.label;
  result["confidence"] = prediction.confidence;
  if (!success) {
    response["error"]["code"] = errorCode;
    response["error"]["message"] = errorMessage;
  }
  emitResponse(response);
}

void sendTinyDecideResponse(const char *id, const TinyDecideResult &decision,
                            bool success, const char *errorCode = nullptr,
                            const char *errorMessage = nullptr) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = success;
  JsonObject routing = response["routing"].to<JsonObject>();
  routing["backend"] = tinyDecide.name();
  routing["mode"] = "model";
  routing["confidence"] = decision.confidence;
  JsonObject result = response["result"].to<JsonObject>();
  result["backend"] = tinyDecide.name();
  result["confidence"] = decision.confidence;
  result["decision_confidence"] = decision.decisionConfidence;
  result["latency_ms"] = decision.elapsedMs;
  result["tokens"] = decision.tokens;
  result["truncated"] = decision.truncated;
  switch (decision.action) {
    case TinyDecideResult::SET_LED: result["intent"] = "led.set"; break;
    case TinyDecideResult::LED_OFF: result["intent"] = "led.off"; break;
    case TinyDecideResult::UNSUPPORTED: result["intent"] = "unsupported"; break;
    default: result["intent"] = "error"; break;
  }
  if (decision.action == TinyDecideResult::SET_LED) {
    result["color_confidence"] = decision.colorConfidence;
    JsonObject args = result["args"].to<JsonObject>();
    args["r"] = decision.r;
    args["g"] = decision.g;
    args["b"] = decision.b;
    args["brightness"] = decision.brightness;
  }
  if (!success) {
    response["error"]["code"] = errorCode;
    response["error"]["message"] = errorMessage;
  }
  emitResponse(response);
}

void sendInfo(const char *id, bool capabilities) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = true;
  JsonObject result = response["result"].to<JsonObject>();
  result["firmware"] = "microneedle-0.4.0";
  result["protocol"] = MICRONEEDLE_PROTOCOL_VERSION;
  result["chip"] = ESP.getChipModel();
  result["cpu_mhz"] = ESP.getCpuFreqMHz();
  result["flash_bytes"] = ESP.getFlashChipSize();
  result["heap_free"] = ESP.getFreeHeap();
  result["psram_bytes"] = ESP.getPsramSize();
  result["psram_free"] = ESP.getFreePsram();
  result["model_backend"] = inferenceReady ? tinyDecide.name() : "ngram-fallback";
  if (capabilities) {
    JsonArray tools = result["tools"].to<JsonArray>();
    JsonArray schemas = result["tool_schemas"].to<JsonArray>();
    for (size_t i = 0; i < ToolRegistry::count(); ++i) {
      const ToolDefinition *definition = ToolRegistry::at(i);
      if (!definition) continue;
      tools.add(definition->name);
      JsonObject schema = schemas.add<JsonObject>();
      schema["name"] = definition->name;
      schema["mutates_hardware"] = (definition->flags & TOOL_MUTATES_HARDWARE) != 0;
      schema["configuration"] = (definition->flags & TOOL_CONFIGURATION) != 0;
      schema["requires_confirmation"] = (definition->flags & TOOL_REQUIRES_CONFIRMATION) != 0;
      JsonArray arguments = schema["args"].to<JsonArray>();
      for (uint8_t argumentIndex = 0; argumentIndex < definition->argCount; ++argumentIndex) {
        const ToolArgRule &rule = definition->args[argumentIndex];
        JsonObject argument = arguments.add<JsonObject>();
        argument["name"] = rule.name;
        argument["type"] = ToolRegistry::argTypeName(rule.type);
        argument["required"] = rule.required;
        if (rule.minimum != INT32_MIN) argument["minimum"] = rule.minimum;
        if (rule.maximum != INT32_MAX) argument["maximum"] = rule.maximum;
        if (rule.allowedValues) {
          JsonArray allowed = argument["allowed"].to<JsonArray>();
          for (uint8_t valueIndex = 0; valueIndex < rule.allowedValueCount; ++valueIndex)
            allowed.add(rule.allowedValues[valueIndex]);
        }
      }
    }
    result["board_profile"] = BoardProfile::name();
    result["device_count"] = deviceRegistry.visibleCount();
    result["task_capacity"] = MICRONEEDLE_MAX_TASKS;
    result["ingress_capacity"] = MICRONEEDLE_INGRESS_CAPACITY;
    result["inference_capacity"] = MICRONEEDLE_INFERENCE_CAPACITY;
  }
  emitResponse(response);
}

bool hasWord(String text, const char *word) {
  text.toLowerCase();
  text.replace('?', ' ');
  text.replace('!', ' ');
  text.replace(',', ' ');
  return (" " + text + " ").indexOf(" " + String(word) + " ") >= 0;
}

bool parseColor(String text, RgbValue &color) {
  if (hasWord(text, "red")) { color = {255, 0, 0}; return true; }
  if (hasWord(text, "green")) { color = {0, 255, 0}; return true; }
  if (hasWord(text, "blue")) { color = {0, 0, 255}; return true; }
  if (hasWord(text, "orange")) { color = {255, 80, 0}; return true; }
  if (hasWord(text, "yellow")) { color = {255, 180, 0}; return true; }
  if (hasWord(text, "purple") || hasWord(text, "violet")) { color = {160, 0, 255}; return true; }
  if (hasWord(text, "pink")) { color = {255, 20, 100}; return true; }
  if (hasWord(text, "white")) { color = {255, 255, 255}; return true; }
  return false;
}

uint8_t parseBrightness(String text, uint8_t fallback) {
  int marker = text.indexOf("brightness");
  if (marker < 0) marker = text.indexOf("bright");
  if (marker < 0) return fallback;
  String number;
  for (size_t i = marker; i < text.length(); ++i) {
    if (isDigit(text[i])) number += text[i];
    else if (number.length()) break;
  }
  if (!number.length()) return fallback;
  return static_cast<uint8_t>(constrain(number.toInt(), 0, 255));
}

bool submitLedColor(const char *id, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness,
                    const char *backend = "deterministic-led-router", float confidence = 1.0f) {
  TaskRequest request;
  request.operation = TaskOperation::DEVICE_SET_RGB;
  strlcpy(request.alias, "status_led", sizeof(request.alias));
  request.r = r;
  request.g = g;
  request.b = b;
  request.brightness = brightness;
  return submitTask(id, request, backend, confidence);
}

bool parsePattern(const String &text, LedPattern &pattern) {
  if (hasWord(text, "blink") || hasWord(text, "blinking")) { pattern = LedPattern::BLINK; return true; }
  if (hasWord(text, "pulse") || hasWord(text, "pulsing")) { pattern = LedPattern::PULSE; return true; }
  if (hasWord(text, "rainbow")) { pattern = LedPattern::RAINBOW; return true; }
  if (hasWord(text, "solid")) { pattern = LedPattern::SOLID; return true; }
  return false;
}

bool validAliasInput(const char *alias) {
  if (!alias) return false;
  const size_t length = strlen(alias);
  if (!length || length >= MICRONEEDLE_ALIAS_SIZE) return false;
  char normalized[MICRONEEDLE_ALIAS_SIZE];
  return DeviceRegistry::normalizeAlias(alias, normalized, sizeof(normalized));
}

bool containsAliasWords(const String &normalized, String alias) {
  alias.toLowerCase();
  alias.replace('_', ' ');
  String paddedText = " " + normalized + " ";
  String paddedAlias = " " + alias + " ";
  return paddedText.indexOf(paddedAlias) >= 0;
}

bool parseUnsignedAfter(const String &text, int start, uint32_t &value) {
  while (start < static_cast<int>(text.length()) && !isDigit(text[start])) ++start;
  if (start >= static_cast<int>(text.length())) return false;
  String number;
  while (start < static_cast<int>(text.length()) && isDigit(text[start])) number += text[start++];
  value = number.toInt();
  return number.length() > 0;
}

PendingInference *reserveInference(const char *id, const String &prompt, uint32_t &jobId) {
  if (!activeIngress) return nullptr;
  for (auto &pending : pendingInference) {
    if (pending.active) continue;
    pending = PendingInference{};
    pending.active = true;
    pending.jobId = nextInferenceId++;
    if (!pending.jobId) pending.jobId = nextInferenceId++;
    pending.ingress = *activeIngress;
    strlcpy(pending.requestId, id ? id : "", sizeof(pending.requestId));
    strlcpy(pending.prompt, prompt.c_str(), sizeof(pending.prompt));
    jobId = pending.jobId;
    return &pending;
  }
  return nullptr;
}

void releaseInference(uint32_t jobId) {
  for (auto &pending : pendingInference)
    if (pending.active && pending.jobId == jobId) pending = PendingInference{};
}

bool queueLedInference(const char *id, const String &prompt) {
  uint32_t jobId = 0;
  if (!reserveInference(id, prompt, jobId)) {
    sendError(id, "inference_capacity_full", "Too many pending inference requests");
    return false;
  }
  InferenceJob job;
  job.id = jobId;
  job.kind = InferenceKind::LED_INTENT;
  strlcpy(job.prompt, prompt.c_str(), sizeof(job.prompt));
  if (!inferenceWorker.submit(job)) {
    releaseInference(jobId);
    sendError(id, "inference_queue_full", "Inference request could not be queued");
    return false;
  }
  return true;
}

bool queueDeviceInference(const char *id, const String &prompt,
                          const DeviceBinding *const *candidates, size_t candidateCount) {
  uint32_t jobId = 0;
  if (!reserveInference(id, prompt, jobId)) {
    sendError(id, "inference_capacity_full", "Too many pending inference requests");
    return false;
  }
  InferenceJob job;
  job.id = jobId;
  job.kind = InferenceKind::DEVICE_ROUTE;
  strlcpy(job.prompt, prompt.c_str(), sizeof(job.prompt));
  job.candidateCount = static_cast<uint8_t>(min(candidateCount, MICRONEEDLE_INFERENCE_CANDIDATES));
  for (uint8_t i = 0; i < job.candidateCount; ++i) {
    strlcpy(job.candidateNames[i], candidates[i]->displayName, sizeof(job.candidateNames[i]));
    strlcpy(job.candidateAliases[i], candidates[i]->alias, sizeof(job.candidateAliases[i]));
  }
  if (!inferenceWorker.submit(job)) {
    releaseInference(jobId);
    sendError(id, "inference_queue_full", "Inference request could not be queued");
    return false;
  }
  return true;
}

bool handleConfigurationPrompt(const char *id, const String &normalized, bool &handled) {
  handled = true;
  if (normalized.startsWith("confirm ")) {
    uint32_t confirmationId = 0;
    if (!parseUnsignedAfter(normalized, 7, confirmationId)) {
      sendError(id, "invalid_confirmation", "Expected: confirm <confirmation_id>");
      return false;
    }
    TaskRequest request;
    const char *error = nullptr;
    if (!confirmations.confirm(confirmationId, currentSessionId(), request, &error)) {
      sendError(id, error, "Confirmation is missing or expired");
      return false;
    }
    return submitTask(id, request);
  }

  if (normalized.startsWith("bind gpio ") || normalized.startsWith("assign gpio ")) {
    const bool bindForm = normalized.startsWith("bind gpio ");
    uint32_t pin = 0;
    if (!parseUnsignedAfter(normalized, bindForm ? 9 : 11, pin) || pin > 255) {
      sendError(id, "invalid_bind_prompt", "Expected: bind GPIO <pin> as <alias>");
      return false;
    }
    String alias;
    if (bindForm) {
      const int asPosition = normalized.indexOf(" as ");
      if (asPosition >= 0) alias = normalized.substring(asPosition + 4);
    } else {
      int aliasPosition = normalized.indexOf(" called ");
      if (aliasPosition >= 0) alias = normalized.substring(aliasPosition + 8);
      else {
        aliasPosition = normalized.indexOf(" to ");
        if (aliasPosition >= 0) alias = normalized.substring(aliasPosition + 4);
      }
    }
    alias.trim();
    if (alias.startsWith("an alias ")) alias.remove(0, 9);
    else if (alias.startsWith("a ")) alias.remove(0, 2);
    else if (alias.startsWith("an ")) alias.remove(0, 3);
    if (!validAliasInput(alias.c_str())) {
      sendError(id, "invalid_alias", "Alias must be 1-31 letters, digits, spaces, dashes, or underscores");
      return false;
    }
    const char *pinReason = nullptr;
    if (!BoardProfile::canBindOutput(static_cast<uint8_t>(pin), &pinReason) ||
        deviceRegistry.pinClaimed(static_cast<uint8_t>(pin))) {
      sendError(id, "unsafe_or_claimed_pin", pinReason ? pinReason : "Pin is already claimed");
      return false;
    }
    TaskRequest request;
    request.operation = TaskOperation::DEVICE_BIND;
    request.priority = TaskPriority::INTERACTIVE;
    request.pin = static_cast<uint8_t>(pin);
    strlcpy(request.alias, alias.c_str(), sizeof(request.alias));
    proposeConfirmation(id, request);
    return false;
  }

  if (normalized.startsWith("remove ") || normalized.startsWith("unbind ")) {
    const int offset = normalized.startsWith("remove ") ? 7 : 7;
    String alias = normalized.substring(offset);
    alias.trim();
    if (!validAliasInput(alias.c_str()) || !deviceRegistry.find(alias.c_str())) {
      sendError(id, "device_not_found", "No registered device has that alias");
      return false;
    }
    TaskRequest request;
    request.operation = TaskOperation::DEVICE_UNBIND;
    strlcpy(request.alias, alias.c_str(), sizeof(request.alias));
    proposeConfirmation(id, request);
    return false;
  }

  if (normalized.startsWith("rename ")) {
    const int toPosition = normalized.indexOf(" to ");
    if (toPosition < 0) {
      sendError(id, "invalid_rename_prompt", "Expected: rename <alias> to <new alias>");
      return false;
    }
    TaskRequest request;
    request.operation = TaskOperation::DEVICE_RENAME;
    String alias = normalized.substring(7, toPosition);
    String newAlias = normalized.substring(toPosition + 4);
    alias.trim();
    newAlias.trim();
    if (!validAliasInput(alias.c_str()) || !deviceRegistry.find(alias.c_str()) ||
        !validAliasInput(newAlias.c_str())) {
      sendError(id, "invalid_alias", "Existing and new aliases must be valid; existing alias must be registered");
      return false;
    }
    strlcpy(request.alias, alias.c_str(), sizeof(request.alias));
    strlcpy(request.newAlias, newAlias.c_str(), sizeof(request.newAlias));
    proposeConfirmation(id, request);
    return false;
  }

  const bool listRequest = normalized == "list devices" ||
                           normalized == "show devices" ||
                           normalized == "give me the devices" ||
                           normalized == "get list of devices" ||
                           normalized == "get list of devices?" ||
                           normalized.indexOf("list of devices") >= 0;
  if (listRequest) {
    sendDeviceList(id);
    return true;
  }

  handled = false;
  return false;
}

bool handleNamedDevicePrompt(const char *id, const String &prompt, const String &normalized,
                             bool &handled) {
  handled = false;
  const DeviceBinding *candidates[DeviceRegistry::bindingSlots()]{};
  size_t candidateCount = 0;
  for (size_t i = 0; i < DeviceRegistry::bindingSlots(); ++i) {
    const DeviceBinding *binding = deviceRegistry.bindingAt(i);
    if (!binding || !binding->occupied) continue;
    String name = binding->displayName;
    String aliasWords = binding->alias;
    if (!containsAliasWords(normalized, name) && !containsAliasWords(normalized, aliasWords)) continue;
    candidates[candidateCount] = binding;
    ++candidateCount;
  }

  // Allow a unique shortened reference such as "my desk" for "desk lamp".
  // It is accepted only when exactly one registered alias contains that whole
  // word, avoiding probabilistic target invention.
  if (!candidateCount) {
    String words = normalized;
    words.replace('?', ' ');
    words.replace('!', ' ');
    int start = 0;
    while (start < static_cast<int>(words.length())) {
      while (start < static_cast<int>(words.length()) && words[start] == ' ') ++start;
      int end = words.indexOf(' ', start);
      if (end < 0) end = words.length();
      String word = words.substring(start, end);
      const bool useful = word.length() >= 3 && word != "turn" && word != "get" &&
                          word != "state" && word != "status" && word != "illuminate" &&
                          word != "switch" && word != "please" && word != "my";
      if (useful) {
        for (size_t i = 0; i < DeviceRegistry::bindingSlots(); ++i) {
          const DeviceBinding *binding = deviceRegistry.bindingAt(i);
          if (!binding || !binding->occupied) continue;
          String aliasWords = binding->alias;
          aliasWords.replace('_', ' ');
          if (containsAliasWords(aliasWords, word)) {
            if (!candidateCount) {
              candidates[0] = binding;
              candidateCount = 1;
            } else if (candidates[0] != binding) {
              candidateCount = 2;  // mark ambiguous
            }
          }
        }
      }
      start = end + 1;
    }
    if (candidateCount > 1) {
      handled = true;
      sendError(id, "ambiguous_device", "Use the full registered device alias");
      return false;
    }
  }
  if (!candidateCount) {
    // Prevent a prompt that names an unregistered semantic device from falling
    // through to the legacy onboard-LED model path.
    const bool deviceLanguage = normalized.indexOf(" lamp") >= 0 ||
                                normalized.indexOf(" light") >= 0 ||
                                normalized.indexOf(" fan") >= 0 ||
                                normalized.startsWith("turn on ") ||
                                normalized.startsWith("turn off ") ||
                                normalized.startsWith("get ");
    if (deviceLanguage) {
      handled = true;
      sendError(id, "device_not_found", "Bind and confirm the named device before controlling it");
    }
    return false;
  }
  handled = true;

  TaskRequest request;
  request.priority = TaskPriority::INTERACTIVE;
  float confidence = 1.0f;
  const DeviceBinding *target = candidates[0];

  // Explicit, safely grounded command phrases do not need probabilistic
  // operation classification. TinyDecide remains available for less-direct
  // phrasing after the deterministic fast path.
  const bool explicitOn = normalized.startsWith("turn on ") ||
                          normalized.startsWith("switch on ") ||
                          normalized.startsWith("enable ") ||
                          normalized.startsWith("illuminate ") ||
                          normalized.startsWith("light up ");
  const bool explicitOff = normalized.startsWith("turn off ") ||
                           normalized.startsWith("switch off ") ||
                           normalized.startsWith("disable ");
  const bool explicitGet = normalized.startsWith("get ") ||
                           normalized.startsWith("read ") ||
                           normalized.indexOf(" state") >= 0 ||
                           normalized.indexOf(" status") >= 0 ||
                           normalized.startsWith("is ");
  if (explicitOn || explicitOff || explicitGet) {
    if (candidateCount != 1) {
      sendError(id, "ambiguous_device", "Name exactly one registered device");
      return false;
    }
    if (explicitGet) request.operation = TaskOperation::DEVICE_GET;
    else {
      request.operation = TaskOperation::DEVICE_SET;
      request.state = explicitOn;
    }
  } else if (inferenceReady) {
    return queueDeviceInference(id, prompt, candidates, candidateCount);
  } else {
    sendError(id, "unsupported_device_prompt", "No supported device operation was found");
    return false;
  }
  strlcpy(request.alias, target->alias, sizeof(request.alias));
  return submitTask(id, request, "deterministic-device-router", confidence);
}

bool handleLedPrompt(const char *id, const String &normalized, bool &handled) {
  handled = normalized.startsWith("led ") || normalized == "led" ||
            normalized.endsWith(" led") || normalized.indexOf(" led ") >= 0 ||
            normalized.indexOf("status led") >= 0 ||
            normalized.indexOf("status light") >= 0 ||
            normalized.indexOf("onboard led") >= 0;
  if (!handled) return false;

  if (hasWord(normalized, "off") || hasWord(normalized, "disable")) {
    TaskRequest request;
    request.operation = TaskOperation::DEVICE_SET;
    strlcpy(request.alias, "status_led", sizeof(request.alias));
    request.state = false;
    return submitTask(id, request, "deterministic-led-router", 1.0f);
  }

  LedPattern pattern;
  if (parsePattern(normalized, pattern)) {
    RgbValue color;
    if (parseColor(normalized, color) &&
        !submitLedColor(id, color.r, color.g, color.b, parseBrightness(normalized, 80))) return false;
    TaskRequest request;
    request.operation = TaskOperation::DEVICE_SET_PATTERN;
    strlcpy(request.alias, "status_led", sizeof(request.alias));
    request.pattern = pattern;
    return submitTask(id, request, "deterministic-led-router", 1.0f);
  }

  RgbValue color;
  if (parseColor(normalized, color))
    return submitLedColor(id, color.r, color.g, color.b, parseBrightness(normalized, 80));

  if (normalized.indexOf("brightness") >= 0 || normalized.indexOf("bright") >= 0) {
    DeviceOperationResult current = deviceRegistry.get("status_led");
    return submitLedColor(id, current.r, current.g, current.b,
                          parseBrightness(normalized, current.brightness));
  }

  if (hasWord(normalized, "on") || hasWord(normalized, "enable")) {
    TaskRequest request;
    request.operation = TaskOperation::DEVICE_SET;
    strlcpy(request.alias, "status_led", sizeof(request.alias));
    request.state = true;
    return submitTask(id, request, "deterministic-led-router", 1.0f);
  }

  sendError(id, "unsupported_color", "Supported colors: red, green, blue, orange, yellow, purple, pink, white");
  return false;
}

bool handleFallbackPrompt(const char *id, String prompt) {
  String normalized = prompt;
  normalized.trim();
  normalized.toLowerCase();
  IntentPrediction prediction = intentClassifier.predict(normalized);
  if (strcmp(prediction.label, "unsupported") == 0 || prediction.confidence < 0.60f) {
    sendPromptResponse(id, prediction, false, "low_confidence",
                       "Prompt intent was not recognized confidently");
    return false;
  }
  if (strcmp(prediction.label, "led.off") == 0 || normalized.indexOf("off") >= 0 ||
      normalized.indexOf("disable") >= 0) {
    TaskRequest request;
    request.operation = TaskOperation::DEVICE_SET;
    strlcpy(request.alias, "status_led", sizeof(request.alias));
    request.state = false;
    return submitTask(id, request, "ngram-fallback", prediction.confidence);
  }
  RgbValue color;
  if (!parseColor(normalized, color)) {
    sendPromptResponse(id, prediction, false, "unsupported_prompt",
                       "No supported LED color or action was found");
    return false;
  }
  return submitLedColor(id, color.r, color.g, color.b, parseBrightness(normalized, 80),
                        "ngram-fallback", prediction.confidence);
}

bool handlePrompt(const char *id, String prompt) {
  String normalized = prompt;
  normalized.trim();
  normalized.toLowerCase();

  bool handled = false;
  const bool ledResult = handleLedPrompt(id, normalized, handled);
  if (handled) return ledResult;
  const bool configurationResult = handleConfigurationPrompt(id, normalized, handled);
  if (handled) return configurationResult;
  const bool deviceResult = handleNamedDevicePrompt(id, prompt, normalized, handled);
  if (handled) return deviceResult;

  if (inferenceReady) return queueLedInference(id, prompt);

  return handleFallbackPrompt(id, prompt);
}

bool getByte(JsonObject args, const char *key, uint8_t &value) {
  if (!args[key].is<int>()) return false;
  int candidate = args[key].as<int>();
  if (candidate < 0 || candidate > 255) return false;
  value = static_cast<uint8_t>(candidate);
  return true;
}

void handleTool(JsonDocument &request, const char *id, const char *tool) {
  const ToolDefinition *definition = ToolRegistry::find(tool);
  if (!definition) {
    sendError(id, "unknown_tool", "Tool is not registered");
    return;
  }
  if (!request["args"].isNull() && !request["args"].is<JsonObject>()) {
    sendError(id, "invalid_args", "args must be an object");
    return;
  }
  JsonObject args = request["args"].as<JsonObject>();
  JsonObjectConst validationArgs = request["args"].as<JsonObjectConst>();
  char validationMessage[96] = {};
  if (!ToolRegistry::validate(*definition, validationArgs, validationMessage,
                              sizeof(validationMessage))) {
    sendError(id, "invalid_args", validationMessage);
    return;
  }

  if (definition->id == ToolId::DEVICE_INFO) { sendInfo(id, false); return; }
  if (definition->id == ToolId::DEVICE_CAPABILITIES) { sendInfo(id, true); return; }
  if (definition->id == ToolId::DEVICE_LIST) { sendDeviceList(id); return; }
  if (definition->id == ToolId::DEVICE_DESCRIBE) {
    if (!args["alias"].is<const char *>()) sendError(id, "invalid_args", "alias is required");
    else sendDeviceDescription(id, args["alias"].as<const char *>());
    return;
  }

  if (definition->id == ToolId::CONFIRMATION_CONFIRM) {
    if (!args["confirmation_id"].is<uint32_t>()) {
      sendError(id, "invalid_args", "confirmation_id is required");
      return;
    }
    TaskRequest pending;
    const char *error = nullptr;
    if (!confirmations.confirm(args["confirmation_id"].as<uint32_t>(), currentSessionId(),
                               pending, &error)) {
      sendError(id, error, "Confirmation is missing or expired");
      return;
    }
    submitTask(id, pending);
    return;
  }

  if (definition->id == ToolId::DEVICE_BIND) {
    if (!args["alias"].is<const char *>() || !args["pin"].is<int>()) {
      sendError(id, "invalid_args", "device.bind requires alias and pin");
      return;
    }
    const char *alias = args["alias"].as<const char *>();
    const int pin = args["pin"].as<int>();
    if (!validAliasInput(alias)) {
      sendError(id, "invalid_alias", "Alias must be 1-31 letters, digits, spaces, dashes, or underscores");
      return;
    }
    if (pin < 0 || pin > 48) {
      sendError(id, "invalid_args", "pin must be a valid ESP32-S3 GPIO from 0 to 48");
      return;
    }
    const char *pinReason = nullptr;
    if (!BoardProfile::canBindOutput(static_cast<uint8_t>(pin), &pinReason) ||
        deviceRegistry.pinClaimed(static_cast<uint8_t>(pin))) {
      sendError(id, "unsafe_or_claimed_pin", pinReason ? pinReason : "Pin is already claimed");
      return;
    }
    TaskRequest task;
    task.operation = TaskOperation::DEVICE_BIND;
    task.pin = static_cast<uint8_t>(pin);
    strlcpy(task.alias, alias, sizeof(task.alias));
    const char *activeLevel = args["active_level"] | "high";
    const char *startupState = args["startup_state"] | "off";
    if ((strcmp(activeLevel, "high") != 0 && strcmp(activeLevel, "low") != 0) ||
        (strcmp(startupState, "on") != 0 && strcmp(startupState, "off") != 0)) {
      sendError(id, "invalid_args", "active_level must be high/low and startup_state must be on/off");
      return;
    }
    task.activeHigh = strcmp(activeLevel, "high") == 0;
    task.startupOn = strcmp(startupState, "on") == 0;
    proposeConfirmation(id, task);
    return;
  }

  if (definition->id == ToolId::DEVICE_UNBIND || definition->id == ToolId::DEVICE_RENAME) {
    if (!args["alias"].is<const char *>()) {
      sendError(id, "invalid_args", "alias is required");
      return;
    }
    const char *alias = args["alias"].as<const char *>();
    if (!validAliasInput(alias) || !deviceRegistry.find(alias)) {
      sendError(id, "device_not_found", "No registered device has that alias");
      return;
    }
    TaskRequest task;
    task.operation = definition->id == ToolId::DEVICE_UNBIND ?
                     TaskOperation::DEVICE_UNBIND : TaskOperation::DEVICE_RENAME;
    strlcpy(task.alias, alias, sizeof(task.alias));
    if (task.operation == TaskOperation::DEVICE_RENAME) {
      if (!args["new_alias"].is<const char *>()) {
        sendError(id, "invalid_args", "new_alias is required");
        return;
      }
      const char *newAlias = args["new_alias"].as<const char *>();
      if (!validAliasInput(newAlias)) {
        sendError(id, "invalid_alias", "new_alias must be 1-31 valid characters");
        return;
      }
      strlcpy(task.newAlias, newAlias, sizeof(task.newAlias));
    }
    proposeConfirmation(id, task);
    return;
  }

  if (definition->id == ToolId::DEVICE_SET || definition->id == ToolId::DEVICE_GET) {
    if (!args["alias"].is<const char *>()) {
      sendError(id, "invalid_args", "alias is required");
      return;
    }
    const char *alias = args["alias"].as<const char *>();
    if (!validAliasInput(alias) || !deviceRegistry.find(alias)) {
      sendError(id, "device_not_found", "No registered device has that alias");
      return;
    }
    TaskRequest task;
    task.operation = definition->id == ToolId::DEVICE_SET ?
                     TaskOperation::DEVICE_SET : TaskOperation::DEVICE_GET;
    strlcpy(task.alias, alias, sizeof(task.alias));
    if (task.operation == TaskOperation::DEVICE_SET) {
      if (args["state"].is<bool>()) task.state = args["state"].as<bool>();
      else if (args["state"].is<const char *>()) {
        const char *state = args["state"].as<const char *>();
        if (strcmp(state, "on") == 0) task.state = true;
        else if (strcmp(state, "off") == 0) task.state = false;
        else {
          sendError(id, "invalid_args", "state must be on, off, true, or false");
          return;
        }
      } else {
        sendError(id, "invalid_args", "device.set requires state");
        return;
      }
    }
    submitTask(id, task);
    return;
  }

  if (definition->id == ToolId::TASK_STATUS) {
    if (!args["task_id"].is<uint32_t>()) {
      sendError(id, "invalid_args", "task_id is required");
      return;
    }
    const TaskRecord *task = taskExecutor.find(args["task_id"].as<uint32_t>());
    if (!task) sendError(id, "task_not_found", "Task does not exist");
    else sendTaskRecord(id, *task);
    return;
  }

  if (definition->id == ToolId::TASK_CANCEL) {
    if (!args["task_id"].is<uint32_t>()) {
      sendError(id, "invalid_args", "task_id is required");
      return;
    }
    const char *error = nullptr;
    if (!taskExecutor.cancel(args["task_id"].as<uint32_t>(), &error))
      sendError(id, error, "Task could not be cancelled");
    else sendSuccess(id);
    return;
  }

  if (definition->id == ToolId::TASK_LIST) {
    JsonDocument response;
    response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
    response["id"] = id;
    response["success"] = true;
    JsonArray tasks = response["result"]["tasks"].to<JsonArray>();
    for (size_t i = 0; i < MICRONEEDLE_MAX_TASKS; ++i) {
      const TaskRecord *task = taskExecutor.taskAt(i);
      if (!task || task->status == TaskStatus::FREE) continue;
      JsonObject item = tasks.add<JsonObject>();
      item["id"] = task->id;
      item["tool"] = TaskExecutor::operationName(task->request.operation);
      item["status"] = TaskExecutor::statusName(task->status);
    }
    emitResponse(response);
    return;
  }

  if (definition->id == ToolId::LED_OFF) {
    TaskRequest task;
    task.operation = TaskOperation::DEVICE_SET;
    strlcpy(task.alias, "status_led", sizeof(task.alias));
    task.state = false;
    submitTask(id, task, "json-tool", 1.0f);
    return;
  }
  if (definition->id == ToolId::LED_SET) {
    uint8_t r, g, b, brightness = 255;
    if (!getByte(args, "r", r) || !getByte(args, "g", g) || !getByte(args, "b", b)) {
      sendError(id, "invalid_args", "led.set requires r, g, and b values from 0 to 255");
      return;
    }
    if (args["brightness"].is<int>() && !getByte(args, "brightness", brightness)) {
      sendError(id, "invalid_args", "brightness must be from 0 to 255");
      return;
    }
    submitLedColor(id, r, g, b, brightness, "json-tool", 1.0f);
    return;
  }
  if (definition->id == ToolId::LED_PATTERN) {
    if (!args["pattern"].is<const char *>()) {
      sendError(id, "invalid_args", "led.pattern requires blink, pulse, rainbow, or solid");
      return;
    }
    LedPattern pattern;
    if (!parsePattern(args["pattern"].as<String>(), pattern)) {
      sendError(id, "invalid_args", "Unsupported LED pattern");
      return;
    }
    TaskRequest task;
    task.operation = TaskOperation::DEVICE_SET_PATTERN;
    strlcpy(task.alias, "status_led", sizeof(task.alias));
    task.pattern = pattern;
    submitTask(id, task, "json-tool", 1.0f);
    return;
  }
  sendError(id, "tool_dispatch_error", "Registered tool has no handler");
}

void handleLine(String line, const char *defaultId = "serial") {
  line.trim();
  if (!line.length()) return;
  if (line.length() > MICRONEEDLE_MAX_INPUT) {
    sendError("", "input_too_large", "Input exceeds the maximum size");
    return;
  }

  if (line[0] != '{') {
    handlePrompt(defaultId, line);
    return;
  }

  JsonDocument request;
  DeserializationError error = deserializeJson(request, line);
  if (error) {
    sendError("", "invalid_json", error.c_str());
    return;
  }
  if (request["v"].is<int>() && request["v"].as<int>() != MICRONEEDLE_PROTOCOL_VERSION) {
    sendError(request["id"] | "", "unsupported_version", "Unsupported protocol version");
    return;
  }

  const char *id = request["id"] | defaultId;
  if (request["prompt"].is<const char *>()) {
    handlePrompt(id, request["prompt"].as<String>());
    return;
  }
  if (request["tool"].is<const char *>()) {
    handleTool(request, id, request["tool"] | "");
    return;
  }
  sendError(id, "invalid_request", "Request requires prompt or tool");
}

void httpSessionId(AsyncWebServerRequest *request, char *output, size_t outputSize) {
  if (request->hasHeader("X-MicroNeedle-Session")) {
    strlcpy(output, request->getHeader("X-MicroNeedle-Session")->value().c_str(), outputSize);
    return;
  }
  String remote = request->client()->remoteIP().toString();
  snprintf(output, outputSize, "http:%s", remote.c_str());
}

bool enqueueHttp(AsyncWebServerRequest *request, const String &payload) {
  if (payload.length() > MICRONEEDLE_MAX_INPUT) {
    request->send(413, "application/json", "{\"success\":false,\"error\":{\"code\":\"input_too_large\"}}");
    return false;
  }
  IngressRequest ingress;
  ingress.origin = IngressOrigin::HTTP_REQUEST;
  strlcpy(ingress.payload, payload.c_str(), sizeof(ingress.payload));
  httpSessionId(request, ingress.session, sizeof(ingress.session));
  ingress.httpRequest = request;
  if (!ingressQueue.push(ingress)) {
    request->send(503, "application/json", "{\"success\":false,\"error\":{\"code\":\"ingress_queue_full\"}}");
    return false;
  }
  return true;
}

PendingInference *findPendingInference(uint32_t jobId) {
  for (auto &pending : pendingInference)
    if (pending.active && pending.jobId == jobId) return &pending;
  return nullptr;
}

void processInferenceResults() {
  InferenceResult result;
  while (inferenceWorker.takeResult(result)) {
    PendingInference *pending = findPendingInference(result.id);
    if (!pending) continue;
    HttpResponseSink httpSink(pending->ingress.httpRequest);
    if (pending->ingress.origin == IngressOrigin::HTTP_REQUEST)
      setResponseContext(&httpSink, pending->ingress.session);
    else
      setResponseContext(&serialResponseSink, pending->ingress.session);

    if (result.kind == InferenceKind::DEVICE_ROUTE) {
      DeviceRouteResult &route = result.device;
      if (route.action == DeviceRouteResult::ERROR || route.action == DeviceRouteResult::UNSUPPORTED ||
          route.targetIndex < 0 || route.targetIndex >= result.candidateCount) {
        sendError(pending->requestId, route.error ? route.error : "unsupported_device_prompt",
                  "Could not confidently route the named-device request");
      } else {
        const char *alias = result.candidateAliases[route.targetIndex];
        const DeviceBinding *binding = deviceRegistry.find(alias);
        if (!binding) {
          sendError(pending->requestId, "device_not_found",
                    "The routed device changed before inference completed");
        } else {
          TaskRequest request;
          strlcpy(request.alias, binding->alias, sizeof(request.alias));
          if (route.action == DeviceRouteResult::SET_ON) {
            request.operation = TaskOperation::DEVICE_SET;
            request.state = true;
          } else if (route.action == DeviceRouteResult::SET_OFF) {
            request.operation = TaskOperation::DEVICE_SET;
            request.state = false;
          } else {
            request.operation = TaskOperation::DEVICE_GET;
          }
          submitTask(pending->requestId, request, tinyDecide.name(),
                     min(route.confidence, route.targetConfidence));
        }
      }
    } else {
      TinyDecideResult &decision = result.led;
      if (decision.action == TinyDecideResult::LED_OFF) {
        TaskRequest request;
        request.operation = TaskOperation::DEVICE_SET;
        strlcpy(request.alias, "status_led", sizeof(request.alias));
        request.state = false;
        submitTask(pending->requestId, request, tinyDecide.name(), decision.confidence);
      } else if (decision.action == TinyDecideResult::SET_LED) {
        submitLedColor(pending->requestId, decision.r, decision.g, decision.b,
                       decision.brightness, tinyDecide.name(), decision.confidence);
      } else if (decision.action == TinyDecideResult::UNSUPPORTED) {
        sendTinyDecideResponse(pending->requestId, decision, false, "unsupported_prompt",
                               decision.error ? decision.error : "TinyDecide rejected the prompt");
      } else {
        handleFallbackPrompt(pending->requestId, String(pending->prompt));
      }
    }
    *pending = PendingInference{};
    setResponseContext(&serialResponseSink, "serial");
  }
}

void processIngress() {
  IngressRequest ingress;
  if (!ingressQueue.pop(ingress)) return;
  activeIngress = &ingress;
  if (ingress.origin == IngressOrigin::HTTP_REQUEST) {
    HttpResponseSink sink(ingress.httpRequest);
    setResponseContext(&sink, ingress.session);
    handleLine(String(ingress.payload), "http");
  } else {
    setResponseContext(&serialResponseSink, ingress.session);
    handleLine(String(ingress.payload), "serial");
  }
  activeIngress = nullptr;
  setResponseContext(&serialResponseSink, "serial");
}

struct HttpBodyBuffer {
  String body;
  bool rejected = false;
};

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("MicroNeedle ESP32-S3 starting");
  deviceRegistry.begin();
  taskExecutor.begin();
  ingressQueue.begin();
  Serial.printf("Device registry: %u binding(s) loaded\n", static_cast<unsigned>(deviceRegistry.count()));
  inferenceReady = tinyDecide.begin() && inferenceWorker.begin();
  if (inferenceReady) {
    Serial.println("Model backend: TinyDecide 10.4M Q4 (asynchronous worker)");
  } else {
    Serial.println("Model backend: n-gram fallback (TinyDecide partition unavailable)");
  }
  Serial.println("Send plain text prompts or versioned JSON lines.");

#if defined(MICRONEEDLE_WIFI_SSID) && defined(MICRONEEDLE_WIFI_PASSWORD)
  WiFi.mode(WIFI_STA);
  WiFi.begin(MICRONEEDLE_WIFI_SSID, MICRONEEDLE_WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts++ < 40) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    webServer.on("/health", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(200, "application/json", "{\"ok\":true,\"protocol\":1}");
    });
    webServer.on("/prompt", HTTP_GET, [](AsyncWebServerRequest *request) {
      if (!request->hasParam("text")) {
        request->send(400, "application/json",
                      "{\"success\":false,\"error\":{\"code\":\"missing_text\"}}");
        return;
      }
      enqueueHttp(request, request->getParam("text")->value());
    });
    webServer.on("/request", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        HttpBodyBuffer *buffer = static_cast<HttpBodyBuffer *>(request->_tempObject);
        if (index == 0) {
          buffer = new HttpBodyBuffer();
          request->_tempObject = buffer;
          if (!buffer || total > MICRONEEDLE_MAX_INPUT) {
            if (buffer) buffer->rejected = true;
            request->send(413, "application/json",
                          "{\"success\":false,\"error\":{\"code\":\"input_too_large\"}}");
          } else {
            buffer->body.reserve(total);
          }
        }
        if (!buffer) return;
        if (!buffer->rejected && buffer->body.length() + len <= MICRONEEDLE_MAX_INPUT)
          buffer->body.concat(reinterpret_cast<const char *>(data), len);
        if (index + len == total) {
          if (!buffer->rejected) enqueueHttp(request, buffer->body);
          delete buffer;
          request->_tempObject = nullptr;
        }
      });
    webServer.addHandler(&taskEvents);
    webServer.begin();
    Serial.print("HTTP prompt endpoint: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/prompt?text=turn%20the%20LED%20orange");
    Serial.println("HTTP JSON endpoint: POST /request; task events: GET /events");
  } else {
    Serial.println("Wi-Fi unavailable; serial prompt input remains active.");
  }
#endif
}

void loop() {
  static String line;
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (line.length()) {
        IngressRequest ingress;
        ingress.origin = IngressOrigin::SERIAL_INPUT;
        strlcpy(ingress.payload, line.c_str(), sizeof(ingress.payload));
        strlcpy(ingress.session, "serial", sizeof(ingress.session));
        if (!ingressQueue.push(ingress)) {
          setResponseContext(&serialResponseSink, "serial");
          sendError("serial", "ingress_queue_full", "Too many pending requests");
        }
        line = "";
      }
    } else if (line.length() <= MICRONEEDLE_MAX_INPUT) {
      line += c;
    }
  }

  processInferenceResults();
  processIngress();
  taskExecutor.tick();
  deviceRegistry.tick();
  TaskRecord event;
  while (taskExecutor.takeEvent(event)) sendTaskRecord(nullptr, event, true);
  delay(2);
}
