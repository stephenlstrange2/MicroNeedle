#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "model_backend.h"
#include "intent_classifier.h"
#include "tinydecide_backend.h"

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
  result["firmware"] = "microneedle-0.1.0";
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
    tools.add("led.pattern");
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

bool handlePrompt(const char *id, String prompt) {
  String normalized = prompt;
  normalized.trim();
  normalized.toLowerCase();

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
  if (tinyDecide.begin()) {
    Serial.println("Model backend: TinyDecide 10.4M Q4");
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
  delay(2);
}
