#pragma once
#include "FreeRTOS.h"
struct StaticSemaphore_t { unsigned owner = 0; };
using SemaphoreHandle_t = StaticSemaphore_t*;
inline SemaphoreHandle_t fake_driver_mutex = nullptr;
inline bool fake_mutex_create_failure = false;
inline bool fake_mutex_take_failure = false;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  if (fake_mutex_create_failure) return nullptr;
  fake_driver_mutex = storage;
  return storage;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) {
  assert(critical_depth == 0);
  if (fake_mutex_take_failure) return 0;
  if (mutex->owner) {
    // Owner must defer, never block or recursively take the driver gate.
    assert(wait == 0);
    return 0;
  }
  mutex->owner = fake_task;
  return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
  assert(mutex->owner == fake_task);
  mutex->owner = 0;
  return pdTRUE;
}
inline void requireDriverGate() {
  assert(fake_driver_mutex && fake_driver_mutex->owner == fake_task);
}
