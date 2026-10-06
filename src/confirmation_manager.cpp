#include "confirmation_manager.h"

#include <esp_system.h>

void ConfirmationManager::expire() {
  const uint32_t now = millis();
  for (auto &entry : pending_) {
    if (entry.active && static_cast<int32_t>(now - entry.expiresAt) > 0) {
      entry = PendingConfirmation{};
    }
  }
}

int ConfirmationManager::find(uint32_t id) const {
  for (size_t i = 0; i < MICRONEEDLE_MAX_CONFIRMATIONS; ++i) {
    if (pending_[i].active && pending_[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

uint32_t ConfirmationManager::propose(const TaskRequest &request, uint32_t ttlMs) {
  expire();
  int slot = -1;
  for (size_t i = 0; i < MICRONEEDLE_MAX_CONFIRMATIONS; ++i) {
    if (!pending_[i].active) { slot = static_cast<int>(i); break; }
  }
  if (slot < 0) return 0;

  uint32_t token = 0;
  do token = esp_random(); while (!token || find(token) >= 0);
  PendingConfirmation &entry = pending_[slot];
  entry.active = true;
  entry.id = token;
  entry.expiresAt = millis() + ttlMs;
  entry.request = request;
  return token;
}

bool ConfirmationManager::confirm(uint32_t id, TaskRequest &request, const char **error) {
  expire();
  const int slot = find(id);
  if (slot < 0) {
    if (error) *error = "confirmation_not_found";
    return false;
  }
  request = pending_[slot].request;
  pending_[slot] = PendingConfirmation{};
  return true;
}

bool ConfirmationManager::reject(uint32_t id, const char **error) {
  expire();
  const int slot = find(id);
  if (slot < 0) {
    if (error) *error = "confirmation_not_found";
    return false;
  }
  pending_[slot] = PendingConfirmation{};
  return true;
}

size_t ConfirmationManager::activeCount() {
  expire();
  size_t count = 0;
  for (const auto &entry : pending_) if (entry.active) ++count;
  return count;
}
