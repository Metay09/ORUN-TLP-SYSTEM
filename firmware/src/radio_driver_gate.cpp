#include "radio_driver_gate.h"
#include <Arduino.h>
#include <semphr.h>

namespace orun_tlp::radio_driver {
namespace {
StaticSemaphore_t storage;
SemaphoreHandle_t mutex = nullptr;
uint32_t operation_generation = 0;
}
bool initialize() {
  if (!mutex) mutex = xSemaphoreCreateMutexStatic(&storage);
  return mutex != nullptr;
}
bool tryAcquire() { return mutex && xSemaphoreTake(mutex, 0) == pdTRUE; }
void acquire() { configASSERT(mutex); xSemaphoreTake(mutex, portMAX_DELAY); }
void release() { xSemaphoreGive(mutex); }
uint32_t generation() { return operation_generation; }
void setGeneration(uint32_t value) { operation_generation = value; }
}  // namespace orun_tlp::radio_driver
