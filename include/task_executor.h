#pragma once

#include <Arduino.h>

#include "device_registry.h"

static constexpr size_t MICRONEEDLE_MAX_TASKS = 16;

enum class TaskPriority : uint8_t { CRITICAL = 0, INTERACTIVE = 1, NORMAL = 2, BACKGROUND = 3 };
enum class TaskStatus : uint8_t { FREE, QUEUED, RUNNING, COMPLETED, FAILED, CANCELLED, TIMED_OUT };
enum class TaskOperation : uint8_t { DEVICE_BIND, DEVICE_UNBIND, DEVICE_RENAME, DEVICE_SET, DEVICE_GET };

struct TaskRequest {
  TaskOperation operation = TaskOperation::DEVICE_GET;
  TaskPriority priority = TaskPriority::INTERACTIVE;
  uint32_t timeoutMs = 2000;
  char alias[MICRONEEDLE_ALIAS_SIZE] = {};
  char newAlias[MICRONEEDLE_ALIAS_SIZE] = {};
  uint8_t pin = 0;
  bool state = false;
  bool activeHigh = true;
  bool startupOn = false;
};

struct TaskRecord {
  uint32_t id = 0;
  TaskRequest request{};
  TaskStatus status = TaskStatus::FREE;
  uint32_t createdAt = 0;
  uint32_t deadline = 0;
  uint32_t completedAt = 0;
  char error[48] = {};
  bool resultState = false;
  bool eventReported = false;
  bool queuedReference = false;
};

class TaskExecutor {
 public:
  explicit TaskExecutor(DeviceRegistry &registry) : registry_(registry) {}

  bool begin();
  bool submit(const TaskRequest &request, uint32_t &taskId, const char **error = nullptr);
  bool cancel(uint32_t taskId, const char **error = nullptr);
  const TaskRecord *find(uint32_t taskId) const;
  const TaskRecord *taskAt(size_t slot) const;
  void tick();
  bool takeEvent(TaskRecord &event);
  size_t activeCount() const;

  static const char *statusName(TaskStatus status);
  static const char *operationName(TaskOperation operation);

 private:
  struct Queue {
    uint8_t slots[MICRONEEDLE_MAX_TASKS]{};
    uint8_t head = 0;
    uint8_t tail = 0;
    uint8_t count = 0;
    bool push(uint8_t slot);
    bool pop(uint8_t &slot);
  };

  DeviceRegistry &registry_;
  TaskRecord tasks_[MICRONEEDLE_MAX_TASKS]{};
  Queue queues_[4]{};
  uint32_t nextId_ = 1;
  uint8_t scheduleCursor_ = 0;
  SemaphoreHandle_t mutex_ = nullptr;

  int reusableSlot() const;
  bool popScheduled(uint8_t &slot);
  void execute(TaskRecord &task);
};
