#include "ingress_queue.h"

namespace {
class Guard {
 public:
  explicit Guard(SemaphoreHandle_t mutex) : mutex_(mutex) {
    locked_ = mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(25)) == pdTRUE;
  }
  ~Guard() { if (locked_) xSemaphoreGive(mutex_); }
  bool locked() const { return locked_; }
 private:
  SemaphoreHandle_t mutex_;
  bool locked_ = false;
};
}  // namespace

bool IngressQueue::begin() {
  if (!mutex_) mutex_ = xSemaphoreCreateMutex();
  return mutex_ != nullptr;
}

bool IngressQueue::push(const IngressRequest &request) {
  Guard guard(mutex_);
  if (!guard.locked() || count_ >= MICRONEEDLE_INGRESS_CAPACITY) return false;
  entries_[tail_] = request;
  tail_ = (tail_ + 1) % MICRONEEDLE_INGRESS_CAPACITY;
  ++count_;
  return true;
}

bool IngressQueue::pop(IngressRequest &request) {
  Guard guard(mutex_);
  if (!guard.locked() || !count_) return false;
  request = entries_[head_];
  entries_[head_] = IngressRequest{};
  head_ = (head_ + 1) % MICRONEEDLE_INGRESS_CAPACITY;
  --count_;
  return true;
}

size_t IngressQueue::size() const {
  Guard guard(mutex_);
  return guard.locked() ? count_ : 0;
}
