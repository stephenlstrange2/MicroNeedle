#include "response_sink.h"

namespace {
SerialResponseSink serialSink;
ResponseSink *activeSink = &serialSink;
char activeSession[48] = "serial";

String serialize(JsonDocument &document) {
  String json;
  serializeJson(document, json);
  return json;
}
}  // namespace

void SerialResponseSink::send(const String &json, int) {
  Serial.println(json);
}

void HttpResponseSink::send(const String &json, int statusCode) {
  if (request_) request_->send(statusCode, "application/json", json);
  request_ = nullptr;
}

void setResponseContext(ResponseSink *sink, const char *sessionId) {
  activeSink = sink ? sink : &serialSink;
  strlcpy(activeSession, sessionId && sessionId[0] ? sessionId : "serial", sizeof(activeSession));
}

const char *currentSessionId() {
  return activeSession;
}

void emitResponse(JsonDocument &document, int statusCode) {
  activeSink->send(serialize(document), statusCode);
}

void emitEvent(JsonDocument &document, AsyncEventSource *events) {
  String json = serialize(document);
  serialSink.send(json, 200);
  if (events) events->send(json.c_str(), "task", millis());
}
