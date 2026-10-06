#include "task_executor.h"

#include <cstring>

namespace {
constexpr uint8_t SCHEDULE[] = {
    0, 0, 0, 0,  // critical
    1, 1, 1,     // interactive
    2, 2,        // normal
    3,           // background
};

bool isTerminal(TaskStatus status) {
  return status == TaskStatus::COMPLETED || status == TaskStatus::FAILED ||
         status == TaskStatus::CANCELLED || status == TaskStatus::TIMED_OUT;
}

void setError(TaskRecord &task, const char *error) {
  strlcpy(task.error, error ? error : "unknown_error", sizeof(task.error));
  task.status = TaskStatus::FAILED;
}

class SemaphoreGuard {
 public:
  explicit SemaphoreGuard(SemaphoreHandle_t mutex) : mutex_(mutex) {
    locked_ = mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
  }
  ~SemaphoreGuard() { if (locked_) xSemaphoreGive(mutex_); }
  bool locked() const { return locked_; }
 private:
  SemaphoreHandle_t mutex_;
  bool locked_ = false;
};
}  // namespace

bool TaskExecutor::begin() {
  if (!mutex_) mutex_ = xSemaphoreCreateMutex();
  return mutex_ != nullptr;
}

bool TaskExecutor::Queue::push(uint8_t slot) {
  if (count >= MICRONEEDLE_MAX_TASKS) return false;
  slots[tail] = slot;
  tail = (tail + 1) % MICRONEEDLE_MAX_TASKS;
  ++count;
  return true;
}

bool TaskExecutor::Queue::pop(uint8_t &slot) {
  if (!count) return false;
  slot = slots[head];
  head = (head + 1) % MICRONEEDLE_MAX_TASKS;
  --count;
  return true;
}

int TaskExecutor::reusableSlot() const {
  // Preserve completed records until all never-used slots are consumed.
  for (size_t i = 0; i < MICRONEEDLE_MAX_TASKS; ++i) {
    if (tasks_[i].status == TaskStatus::FREE) return static_cast<int>(i);
  }
  int oldest = -1;
  uint32_t oldestAge = 0;
  const uint32_t now = millis();
  for (size_t i = 0; i < MICRONEEDLE_MAX_TASKS; ++i) {
    const TaskRecord &task = tasks_[i];
    if (!isTerminal(task.status) || !task.eventReported || task.queuedReference) continue;
    const uint32_t age = now - task.completedAt;
    if (oldest < 0 || age > oldestAge) {
      oldest = static_cast<int>(i);
      oldestAge = age;
    }
  }
  return oldest;
}

bool TaskExecutor::submit(const TaskRequest &request, uint32_t &taskId, const char **error) {
  SemaphoreGuard guard(mutex_);
  if (!guard.locked()) {
    if (error) *error = "executor_not_ready";
    return false;
  }
  const int slot = reusableSlot();
  if (slot < 0) {
    if (error) *error = "task_pool_full";
    return false;
  }
  const uint8_t priority = static_cast<uint8_t>(request.priority);
  if (priority >= 4) {
    if (error) *error = "invalid_priority";
    return false;
  }

  TaskRecord &task = tasks_[slot];
  task = TaskRecord{};
  task.id = nextId_++;
  if (!task.id) task.id = nextId_++;
  task.request = request;
  task.status = TaskStatus::QUEUED;
  task.createdAt = millis();
  task.deadline = task.createdAt + (request.timeoutMs ? request.timeoutMs : 2000);
  task.queuedReference = true;
  if (!queues_[priority].push(static_cast<uint8_t>(slot))) {
    task = TaskRecord{};
    if (error) *error = "priority_queue_full";
    return false;
  }
  taskId = task.id;
  return true;
}

bool TaskExecutor::cancel(uint32_t taskId, const char **error) {
  SemaphoreGuard guard(mutex_);
  if (!guard.locked()) {
    if (error) *error = "executor_not_ready";
    return false;
  }
  for (auto &task : tasks_) {
    if (task.id != taskId || task.status == TaskStatus::FREE) continue;
    if (task.status != TaskStatus::QUEUED) {
      if (error) *error = isTerminal(task.status) ? "task_already_finished" : "task_running";
      return false;
    }
    task.status = TaskStatus::CANCELLED;
    task.completedAt = millis();
    return true;
  }
  if (error) *error = "task_not_found";
  return false;
}

const TaskRecord *TaskExecutor::find(uint32_t taskId) const {
  for (const auto &task : tasks_) {
    if (task.id == taskId && task.status != TaskStatus::FREE) return &task;
  }
  return nullptr;
}

const TaskRecord *TaskExecutor::taskAt(size_t slot) const {
  return slot < MICRONEEDLE_MAX_TASKS ? &tasks_[slot] : nullptr;
}

bool TaskExecutor::popScheduled(uint8_t &slot) {
  for (size_t attempt = 0; attempt < sizeof(SCHEDULE); ++attempt) {
    const uint8_t priority = SCHEDULE[scheduleCursor_++ % sizeof(SCHEDULE)];
    if (queues_[priority].pop(slot)) return true;
  }
  for (auto &queue : queues_) if (queue.pop(slot)) return true;
  return false;
}

void TaskExecutor::tick() {
  SemaphoreGuard guard(mutex_);
  if (!guard.locked()) return;
  uint8_t slot = 0;
  if (!popScheduled(slot)) return;
  TaskRecord &task = tasks_[slot];
  task.queuedReference = false;
  if (task.status == TaskStatus::CANCELLED) return;
  if (static_cast<int32_t>(millis() - task.deadline) > 0) {
    task.status = TaskStatus::TIMED_OUT;
    task.completedAt = millis();
    strlcpy(task.error, "deadline_exceeded", sizeof(task.error));
    return;
  }
  task.status = TaskStatus::RUNNING;
  execute(task);
  task.completedAt = millis();
  // Current device handlers are short and synchronous. Report an overrun, but
  // do not pretend that already-applied side effects were rolled back.
  if (static_cast<int32_t>(task.completedAt - task.deadline) > 0 &&
      task.status == TaskStatus::COMPLETED) {
    strlcpy(task.error, "completed_after_deadline", sizeof(task.error));
  }
}

void TaskExecutor::execute(TaskRecord &task) {
  DeviceOperationResult result;
  switch (task.request.operation) {
    case TaskOperation::DEVICE_BIND:
      result = registry_.bindGpioOutput(task.request.alias, task.request.pin,
                                        task.request.activeHigh, task.request.startupOn);
      break;
    case TaskOperation::DEVICE_UNBIND:
      result = registry_.unbind(task.request.alias);
      break;
    case TaskOperation::DEVICE_RENAME:
      result = registry_.rename(task.request.alias, task.request.newAlias);
      break;
    case TaskOperation::DEVICE_SET:
      result = registry_.set(task.request.alias, task.request.state);
      break;
    case TaskOperation::DEVICE_GET:
      result = registry_.get(task.request.alias);
      break;
  }
  task.resultState = result.state;
  if (result.success) task.status = TaskStatus::COMPLETED;
  else setError(task, result.error);
}

bool TaskExecutor::takeEvent(TaskRecord &event) {
  SemaphoreGuard guard(mutex_);
  if (!guard.locked()) return false;
  for (auto &task : tasks_) {
    if (isTerminal(task.status) && !task.eventReported) {
      event = task;
      task.eventReported = true;
      return true;
    }
  }
  return false;
}

size_t TaskExecutor::activeCount() const {
  size_t count = 0;
  for (const auto &task : tasks_) {
    if (task.status == TaskStatus::QUEUED || task.status == TaskStatus::RUNNING) ++count;
  }
  return count;
}

const char *TaskExecutor::statusName(TaskStatus status) {
  switch (status) {
    case TaskStatus::FREE: return "free";
    case TaskStatus::QUEUED: return "queued";
    case TaskStatus::RUNNING: return "running";
    case TaskStatus::COMPLETED: return "completed";
    case TaskStatus::FAILED: return "failed";
    case TaskStatus::CANCELLED: return "cancelled";
    case TaskStatus::TIMED_OUT: return "timed_out";
  }
  return "unknown";
}

const char *TaskExecutor::operationName(TaskOperation operation) {
  switch (operation) {
    case TaskOperation::DEVICE_BIND: return "device.bind";
    case TaskOperation::DEVICE_UNBIND: return "device.unbind";
    case TaskOperation::DEVICE_RENAME: return "device.rename";
    case TaskOperation::DEVICE_SET: return "device.set";
    case TaskOperation::DEVICE_GET: return "device.get";
  }
  return "unknown";
}
