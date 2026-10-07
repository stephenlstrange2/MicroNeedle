#pragma once

#include <Arduino.h>

#include "task_executor.h"

static constexpr size_t MICRONEEDLE_MAX_CONFIRMATIONS = 4;

struct PendingConfirmation {
  bool active = false;
  uint32_t id = 0;
  uint32_t expiresAt = 0;
  uint32_t operationDigest = 0;
  char session[48] = {};
  TaskRequest request{};
};

class ConfirmationManager {
 public:
  uint32_t propose(const TaskRequest &request, const char *session, uint32_t ttlMs = 30000);
  bool confirm(uint32_t id, const char *session, TaskRequest &request,
               const char **error = nullptr);
  bool reject(uint32_t id, const char *session, const char **error = nullptr);
  size_t activeCount();

 private:
  PendingConfirmation pending_[MICRONEEDLE_MAX_CONFIRMATIONS]{};
  void expire();
  int find(uint32_t id) const;
  static uint32_t digest(const TaskRequest &request);
};
