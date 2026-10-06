#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "model_backend.h"
#include "intent_classifier.h"
#include "tinydecide_backend.h"
#include "device_registry.h"
#include "task_executor.h"
#include "confirmation_manager.h"

#ifndef KM_RGB_PIN
#define KM_RGB_PIN 48
#endif

#ifndef MICRONEEDLE_PROTOCOL_VERSION
#define MICRONEEDLE_PROTOCOL_VERSION 1
#endif

static constexpr size_t MICRONEEDLE_MAX_INPUT = 768;
static constexpr uint16_t LED_COUNT = 1;

Adafruit_NeoPixel pixel(LED_COUNT, KM_RGB_PIN, NEO_GRB + NEO_KHZ800);
AsyncWebServer webServer(80);
IntentClassifier intentClassifier;
TinyDecideBackend tinyDecide;
DeviceRegistry deviceRegistry;
TaskExecutor taskExecutor(deviceRegistry);
ConfirmationManager confirmations;

struct RgbValue {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

void setStatus(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 32) {
  pixel.setBrightness(brightness);
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void sendError(const char *id, const char *code, const char *message) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = false;
  response["error"]["code"] = code;
  response["error"]["message"] = message;
  serializeJson(response, Serial);
  Serial.println();
}

void sendSuccess(const char *id) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = true;
  serializeJson(response, Serial);
  Serial.println();
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
  if (backend) response["model"]["backend"] = backend;
  if (confidence >= 0.0f) response["model"]["confidence"] = confidence;
  serializeJson(response, Serial);
  Serial.println();
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
  serializeJson(response, Serial);
  Serial.println();
}

bool proposeConfirmation(const char *id, const TaskRequest &request) {
  const uint32_t confirmationId = confirmations.propose(request);
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
  for (size_t i = 0; i < MICRONEEDLE_MAX_DEVICES; ++i) {
    const DeviceBinding *binding = deviceRegistry.bindingAt(i);
    if (!binding || !binding->occupied) continue;
    JsonObject device = devices.add<JsonObject>();
    device["id"] = binding->id;
    device["alias"] = binding->alias;
    device["display_name"] = binding->displayName;
    device["driver"] = "gpio.output";
    device["pin"] = binding->pin;
    device["active_level"] = binding->activeHigh ? "high" : "low";
    device["state"] = binding->currentOn ? "on" : "off";
  }
  serializeJson(response, Serial);
  Serial.println();
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
  device["driver"] = "gpio.output";
  device["pin"] = binding->pin;
  device["active_level"] = binding->activeHigh ? "high" : "low";
  device["startup_state"] = binding->startupOn ? "on" : "off";
  device["state"] = binding->currentOn ? "on" : "off";
  JsonArray capabilities = device["capabilities"].to<JsonArray>();
  if (binding->capabilities & CAP_ON) capabilities.add("on");
  if (binding->capabilities & CAP_OFF) capabilities.add("off");
  if (binding->capabilities & CAP_STATE) capabilities.add("state");
  serializeJson(response, Serial);
  Serial.println();
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
       record.request.operation == TaskOperation::DEVICE_SET)) {
    task["result"]["state"] = record.resultState ? "on" : "off";
  }
  if (record.error[0]) task["error"] = record.error;
  serializeJson(response, Serial);
  Serial.println();
}

void sendPromptResponse(const char *id, const IntentPrediction &prediction,
                        bool success, const char *errorCode = nullptr,
                        const char *errorMessage = nullptr) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = success;
  JsonObject result = response["result"].to<JsonObject>();
  result["intent"] = prediction.label;
  result["confidence"] = prediction.confidence;
  if (!success) {
    response["error"]["code"] = errorCode;
    response["error"]["message"] = errorMessage;
  }
  serializeJson(response, Serial);
  Serial.println();
}

void sendTinyDecideResponse(const char *id, const TinyDecideResult &decision,
                            bool success, const char *errorCode = nullptr,
                            const char *errorMessage = nullptr) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = success;
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
  serializeJson(response, Serial);
  Serial.println();
}

void sendInfo(const char *id, bool capabilities) {
  JsonDocument response;
  response["v"] = MICRONEEDLE_PROTOCOL_VERSION;
  response["id"] = id ? id : "";
  response["success"] = true;
  JsonObject result = response["result"].to<JsonObject>();
  result["firmware"] = "microneedle-0.2.0";
  result["protocol"] = MICRONEEDLE_PROTOCOL_VERSION;
  result["chip"] = ESP.getChipModel();
  result["cpu_mhz"] = ESP.getCpuFreqMHz();
  result["flash_bytes"] = ESP.getFlashChipSize();
  result["heap_free"] = ESP.getFreeHeap();
  result["psram_bytes"] = ESP.getPsramSize();
  result["psram_free"] = ESP.getFreePsram();
  result["model_backend"] = tinyDecide.ready() ? tinyDecide.name() : "ngram-fallback";
  if (capabilities) {
    JsonArray tools = result["tools"].to<JsonArray>();
    tools.add("device.info");
    tools.add("device.capabilities");
    tools.add("led.set");
    tools.add("led.off");
    tools.add("device.bind");
    tools.add("device.unbind");
    tools.add("device.rename");
    tools.add("device.list");
    tools.add("device.describe");
    tools.add("device.get");
    tools.add("device.set");
    tools.add("task.status");
    tools.add("task.cancel");
    tools.add("task.list");
    tools.add("confirmation.confirm");
    result["board_profile"] = BoardProfile::name();
    result["device_count"] = deviceRegistry.count();
    result["task_capacity"] = MICRONEEDLE_MAX_TASKS;
  }
  serializeJson(response, Serial);
  Serial.println();
}

bool parseColor(String text, RgbValue &color) {
  text.toLowerCase();
  if (text.indexOf("red") >= 0) { color = {255, 0, 0}; return true; }
  if (text.indexOf("green") >= 0) { color = {0, 255, 0}; return true; }
  if (text.indexOf("blue") >= 0) { color = {0, 0, 255}; return true; }
  if (text.indexOf("orange") >= 0) { color = {255, 80, 0}; return true; }
  if (text.indexOf("yellow") >= 0) { color = {255, 180, 0}; return true; }
  if (text.indexOf("purple") >= 0 || text.indexOf("violet") >= 0) { color = {160, 0, 255}; return true; }
  if (text.indexOf("pink") >= 0) { color = {255, 20, 100}; return true; }
  if (text.indexOf("white") >= 0) { color = {255, 255, 255}; return true; }
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

bool validateAndSetLed(int r, int g, int b, int brightness) {
  if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255 ||
      brightness < 0 || brightness > 255) {
    return false;
  }
  setStatus(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
            static_cast<uint8_t>(b), static_cast<uint8_t>(brightness));
  return true;
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
    if (!confirmations.confirm(confirmationId, request, &error)) {
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

  if (normalized == "list devices" || normalized == "show devices") {
    sendDeviceList(id);
    return true;
  }

  handled = false;
  return false;
}

bool handleNamedDevicePrompt(const char *id, const String &prompt, const String &normalized,
                             bool &handled) {
  handled = false;
  const DeviceBinding *candidates[MICRONEEDLE_MAX_DEVICES]{};
  const char *names[MICRONEEDLE_MAX_DEVICES]{};
  size_t candidateCount = 0;
  for (size_t i = 0; i < MICRONEEDLE_MAX_DEVICES; ++i) {
    const DeviceBinding *binding = deviceRegistry.bindingAt(i);
    if (!binding || !binding->occupied) continue;
    String name = binding->displayName;
    String aliasWords = binding->alias;
    if (!containsAliasWords(normalized, name) && !containsAliasWords(normalized, aliasWords)) continue;
    candidates[candidateCount] = binding;
    names[candidateCount] = binding->displayName;
    ++candidateCount;
  }
  if (!candidateCount) return false;
  handled = true;

  TaskRequest request;
  request.priority = TaskPriority::INTERACTIVE;
  float confidence = 1.0f;
  const DeviceBinding *target = candidates[0];
  if (tinyDecide.ready()) {
    DeviceRouteResult route = tinyDecide.routeDevice(prompt, names, candidateCount);
    if (route.action == DeviceRouteResult::ERROR || route.action == DeviceRouteResult::UNSUPPORTED ||
        route.targetIndex < 0 || static_cast<size_t>(route.targetIndex) >= candidateCount) {
      sendError(id, route.error ? route.error : "unsupported_device_prompt",
                "Could not confidently route the named-device request");
      return false;
    }
    target = candidates[route.targetIndex];
    confidence = min(route.confidence, route.targetConfidence);
    if (route.action == DeviceRouteResult::SET_ON) {
      request.operation = TaskOperation::DEVICE_SET;
      request.state = true;
    } else if (route.action == DeviceRouteResult::SET_OFF) {
      request.operation = TaskOperation::DEVICE_SET;
      request.state = false;
    } else {
      request.operation = TaskOperation::DEVICE_GET;
    }
  } else {
    if (normalized.indexOf("turn on") >= 0 || normalized.indexOf("switch on") >= 0) {
      request.operation = TaskOperation::DEVICE_SET;
      request.state = true;
    } else if (normalized.indexOf("turn off") >= 0 || normalized.indexOf("switch off") >= 0) {
      request.operation = TaskOperation::DEVICE_SET;
      request.state = false;
    } else if (normalized.indexOf("state") >= 0 || normalized.indexOf("status") >= 0) {
      request.operation = TaskOperation::DEVICE_GET;
    } else {
      sendError(id, "unsupported_device_prompt", "No supported device operation was found");
      return false;
    }
  }
  strlcpy(request.alias, target->alias, sizeof(request.alias));
  return submitTask(id, request, tinyDecide.ready() ? tinyDecide.name() : "deterministic-device-router",
                    confidence);
}

bool handlePrompt(const char *id, String prompt) {
  String normalized = prompt;
  normalized.trim();
  normalized.toLowerCase();

  bool handled = false;
  const bool configurationResult = handleConfigurationPrompt(id, normalized, handled);
  if (handled) return configurationResult;
  const bool deviceResult = handleNamedDevicePrompt(id, prompt, normalized, handled);
  if (handled) return deviceResult;

  if (tinyDecide.ready()) {
    TinyDecideResult decision = tinyDecide.interpret(prompt);
    if (decision.action == TinyDecideResult::LED_OFF) {
      validateAndSetLed(0, 0, 0, 0);
      sendTinyDecideResponse(id, decision, true);
      return true;
    }
    if (decision.action == TinyDecideResult::SET_LED) {
      if (!validateAndSetLed(decision.r, decision.g, decision.b, decision.brightness)) {
        sendTinyDecideResponse(id, decision, false, "invalid_model_output",
                               "Model output failed command validation");
        return false;
      }
      sendTinyDecideResponse(id, decision, true);
      return true;
    }
    if (decision.action == TinyDecideResult::UNSUPPORTED) {
      sendTinyDecideResponse(id, decision, false, "unsupported_prompt",
                             decision.error ? decision.error : "TinyDecide rejected the prompt");
      return false;
    }
    // Runtime failures fall through to the small local classifier.
  }

  IntentPrediction prediction = intentClassifier.predict(normalized);
  if (strcmp(prediction.label, "unsupported") == 0 || prediction.confidence < 0.60f) {
    sendPromptResponse(id, prediction, false, "low_confidence", "Prompt intent was not recognized confidently");
    return false;
  }
  if (strcmp(prediction.label, "led.off") == 0 || normalized.indexOf("off") >= 0 || normalized.indexOf("disable") >= 0) {
    validateAndSetLed(0, 0, 0, 0);
    sendPromptResponse(id, prediction, true);
    return true;
  }

  RgbValue color;
  if (!parseColor(normalized, color)) {
    sendPromptResponse(id, prediction, false, "unsupported_prompt", "No supported LED color or action was found");
    return false;
  }

  uint8_t brightness = parseBrightness(normalized, 80);
  validateAndSetLed(color.r, color.g, color.b, brightness);
  sendPromptResponse(id, prediction, true);
  return true;
}

bool getByte(JsonObject args, const char *key, uint8_t &value) {
  if (!args[key].is<int>()) return false;
  int candidate = args[key].as<int>();
  if (candidate < 0 || candidate > 255) return false;
  value = static_cast<uint8_t>(candidate);
  return true;
}

void handleTool(JsonDocument &request, const char *id, const char *tool) {
  JsonObject args = request["args"].as<JsonObject>();
  if (strcmp(tool, "device.info") == 0) { sendInfo(id, false); return; }
  if (strcmp(tool, "device.capabilities") == 0) { sendInfo(id, true); return; }
  if (strcmp(tool, "device.list") == 0) { sendDeviceList(id); return; }
  if (strcmp(tool, "device.describe") == 0) {
    if (!args["alias"].is<const char *>()) sendError(id, "invalid_args", "alias is required");
    else sendDeviceDescription(id, args["alias"].as<const char *>());
    return;
  }

  if (strcmp(tool, "confirmation.confirm") == 0) {
    if (!args["confirmation_id"].is<uint32_t>()) {
      sendError(id, "invalid_args", "confirmation_id is required");
      return;
    }
    TaskRequest pending;
    const char *error = nullptr;
    if (!confirmations.confirm(args["confirmation_id"].as<uint32_t>(), pending, &error)) {
      sendError(id, error, "Confirmation is missing or expired");
      return;
    }
    submitTask(id, pending);
    return;
  }

  if (strcmp(tool, "device.bind") == 0) {
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

  if (strcmp(tool, "device.unbind") == 0 || strcmp(tool, "device.rename") == 0) {
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
    task.operation = strcmp(tool, "device.unbind") == 0 ?
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

  if (strcmp(tool, "device.set") == 0 || strcmp(tool, "device.get") == 0) {
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
    task.operation = strcmp(tool, "device.set") == 0 ?
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

  if (strcmp(tool, "task.status") == 0) {
    if (!args["task_id"].is<uint32_t>()) {
      sendError(id, "invalid_args", "task_id is required");
      return;
    }
    const TaskRecord *task = taskExecutor.find(args["task_id"].as<uint32_t>());
    if (!task) sendError(id, "task_not_found", "Task does not exist");
    else sendTaskRecord(id, *task);
    return;
  }

  if (strcmp(tool, "task.cancel") == 0) {
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

  if (strcmp(tool, "task.list") == 0) {
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
    serializeJson(response, Serial);
    Serial.println();
    return;
  }

  if (strcmp(tool, "led.off") == 0) {
    validateAndSetLed(0, 0, 0, 0);
    sendSuccess(id);
    return;
  }
  if (strcmp(tool, "led.set") == 0) {
    uint8_t r, g, b, brightness = 255;
    if (!getByte(args, "r", r) || !getByte(args, "g", g) || !getByte(args, "b", b)) {
      sendError(id, "invalid_args", "led.set requires r, g, and b values from 0 to 255");
      return;
    }
    if (args["brightness"].is<int>() && !getByte(args, "brightness", brightness)) {
      sendError(id, "invalid_args", "brightness must be from 0 to 255");
      return;
    }
    validateAndSetLed(r, g, b, brightness);
    sendSuccess(id);
    return;
  }
  sendError(id, "unknown_tool", "Tool is not registered");
}

void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
  if (line.length() > MICRONEEDLE_MAX_INPUT) {
    sendError("", "input_too_large", "Input exceeds the maximum size");
    return;
  }

  if (line[0] != '{') {
    setStatus(20, 20, 80);
    handlePrompt("serial", line);
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

  const char *id = request["id"] | "serial";
  if (request["prompt"].is<const char *>()) {
    setStatus(20, 20, 80);
    handlePrompt(id, request["prompt"].as<String>());
    return;
  }
  if (request["tool"].is<const char *>()) {
    handleTool(request, id, request["tool"] | "");
    return;
  }
  sendError(id, "invalid_request", "Request requires prompt or tool");
}

void setup() {
  Serial.begin(115200);
  delay(250);
  pixel.begin();
  pixel.clear();
  pixel.show();
  setStatus(0, 0, 80);
  Serial.println("MicroNeedle ESP32-S3 starting");
  deviceRegistry.begin();
  taskExecutor.begin();
  Serial.printf("Device registry: %u binding(s) loaded\n", static_cast<unsigned>(deviceRegistry.count()));
  if (tinyDecide.begin()) {
    Serial.println("Model backend: TinyDecide 10.4M Q4");
  } else {
    Serial.println("Model backend: n-gram fallback (TinyDecide partition unavailable)");
  }
  Serial.println("Send plain text prompts or versioned JSON lines.");

#if defined(MICRONEEDLE_WIFI_SSID) && defined(MICRONEEDLE_WIFI_PASSWORD) && \
    defined(MICRONEEDLE_EXPERIMENTAL_HTTP_PROMPT)
  // Experimental only: these handlers run on the AsyncWebServer task. Keep
  // disabled until requests/responses are moved through the bounded owner-loop
  // ingress queue, so model inference, NVS, Serial, and GPIO remain single-owner.
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
        request->send(400, "application/json", "{\"success\":false,\"error\":\"missing_text\"}");
        return;
      }
      String prompt = request->getParam("text")->value();
      bool accepted = handlePrompt("http", prompt);
      request->send(accepted ? 200 : 422, "application/json",
                    accepted ? "{\"success\":true}" : "{\"success\":false,\"error\":\"unsupported_prompt\"}");
    });
    webServer.on("/prompt", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index != 0 || total > MICRONEEDLE_MAX_INPUT) {
          if (index == 0) request->send(413, "application/json", "{\"success\":false,\"error\":\"input_too_large\"}");
          return;
        }
        JsonDocument body;
        if (deserializeJson(body, data, len) || !body["prompt"].is<const char *>()) {
          request->send(400, "application/json", "{\"success\":false,\"error\":\"invalid_prompt_json\"}");
          return;
        }
        bool accepted = handlePrompt("http", body["prompt"].as<String>());
        request->send(accepted ? 200 : 422, "application/json",
                      accepted ? "{\"success\":true}" : "{\"success\":false,\"error\":\"prompt_rejected\"}");
      });
    webServer.begin();
    Serial.print("HTTP prompt endpoint: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/prompt?text=turn%20the%20LED%20orange");
  } else {
    Serial.println("Wi-Fi unavailable; serial prompt input remains active.");
  }
#endif

  setStatus(0, 80, 0);
}

void loop() {
  static String line;
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (line.length()) {
        handleLine(line);
        line = "";
      }
    } else if (line.length() <= MICRONEEDLE_MAX_INPUT) {
      line += c;
    }
  }

  taskExecutor.tick();
  TaskRecord event;
  while (taskExecutor.takeEvent(event)) {
    sendTaskRecord(nullptr, event, true);
    if (event.status == TaskStatus::COMPLETED) setStatus(0, 80, 0);
    else setStatus(80, 0, 0);
  }
  delay(2);
}
