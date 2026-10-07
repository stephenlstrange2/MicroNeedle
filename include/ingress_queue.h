#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

static constexpr size_t MICRONEEDLE_INGRESS_CAPACITY = 8;
static constexpr size_t MICRONEEDLE_INGRESS_PAYLOAD_SIZE = 769;

enum class IngressOrigin : uint8_t { SERIAL_INPUT, HTTP_REQUEST };

struct IngressRequest {
  IngressOrigin origin = IngressOrigin::SERIAL_INPUT;
  char payload[MICRONEEDLE_INGRESS_PAYLOAD_SIZE] = {};
  char session[48] = {};
  AsyncWebServerRequest *httpRequest = nullptr;
};

class IngressQueue {
 public:
  bool begin();
  bool push(const IngressRequest &request);
  bool pop(IngressRequest &request);
  size_t size() const;

 private:
  IngressRequest entries_[MICRONEEDLE_INGRESS_CAPACITY]{};
  uint8_t head_ = 0;
  uint8_t tail_ = 0;
  uint8_t count_ = 0;
  SemaphoreHandle_t mutex_ = nullptr;
};
