#include "inference_worker.h"

bool InferenceWorker::begin() {
  if (!jobs_) jobs_ = xQueueCreate(MICRONEEDLE_INFERENCE_CAPACITY, sizeof(InferenceJob));
  if (!results_) results_ = xQueueCreate(MICRONEEDLE_INFERENCE_CAPACITY, sizeof(InferenceResult));
  if (!jobs_ || !results_) return false;
  if (!task_) {
    if (xTaskCreatePinnedToCore(taskEntry, "mn_inference", 16384, this, 1, &task_, 0) != pdPASS)
      return false;
  }
  return true;
}

bool InferenceWorker::submit(const InferenceJob &job) {
  return jobs_ && xQueueSend(jobs_, &job, 0) == pdTRUE;
}

bool InferenceWorker::takeResult(InferenceResult &result) {
  return results_ && xQueueReceive(results_, &result, 0) == pdTRUE;
}

void InferenceWorker::taskEntry(void *context) {
  static_cast<InferenceWorker *>(context)->run();
}

void InferenceWorker::run() {
  InferenceJob job;
  for (;;) {
    if (xQueueReceive(jobs_, &job, portMAX_DELAY) != pdTRUE) continue;
    InferenceResult result;
    result.id = job.id;
    result.kind = job.kind;
    result.candidateCount = job.candidateCount;
    memcpy(result.candidateAliases, job.candidateAliases, sizeof(result.candidateAliases));
    if (job.kind == InferenceKind::LED_INTENT) {
      result.led = backend_.interpret(String(job.prompt));
    } else {
      const char *names[MICRONEEDLE_INFERENCE_CANDIDATES]{};
      for (uint8_t i = 0; i < job.candidateCount; ++i) names[i] = job.candidateNames[i];
      result.device = backend_.routeDevice(String(job.prompt), names, job.candidateCount);
    }
    // The owner loop must eventually consume every result. Wait here rather
    // than silently dropping a model decision when the bounded result queue is full.
    xQueueSend(results_, &result, portMAX_DELAY);
  }
}
