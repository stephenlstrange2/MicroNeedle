#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

class ResponseSink {
 public:
  virtual ~ResponseSink() = default;
  virtual void send(const String &json, int statusCode) = 0;
};

class SerialResponseSink final : public ResponseSink {
 public:
  void send(const String &json, int statusCode) override;
};

class HttpResponseSink final : public ResponseSink {
 public:
  explicit HttpResponseSink(AsyncWebServerRequest *request) : request_(request) {}
  void send(const String &json, int statusCode) override;

 private:
  AsyncWebServerRequest *request_;
};

void setResponseContext(ResponseSink *sink, const char *sessionId);
const char *currentSessionId();
void emitResponse(JsonDocument &document, int statusCode = 200);
void emitEvent(JsonDocument &document, AsyncEventSource *events = nullptr);
