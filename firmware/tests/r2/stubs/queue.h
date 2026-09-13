#pragma once

#include <string.h>

#include "FreeRTOS.h"

struct StaticQueue_t {
  uint8_t* storage = nullptr;
  UBaseType_t capacity = 0;
  UBaseType_t item_size = 0;
  UBaseType_t head = 0;
  UBaseType_t tail = 0;
  UBaseType_t count = 0;
};

using QueueHandle_t = StaticQueue_t*;

inline QueueHandle_t xQueueCreateStatic(UBaseType_t capacity,
                                        UBaseType_t item_size,
                                        uint8_t* storage,
                                        StaticQueue_t* queue) {
  if (capacity == 0 || item_size == 0 || storage == nullptr || queue == nullptr)
    return nullptr;
  *queue = {};
  queue->storage = storage;
  queue->capacity = capacity;
  queue->item_size = item_size;
  return queue;
}

inline BaseType_t xQueueReset(QueueHandle_t queue) {
  if (queue == nullptr) return 0;
  queue->head = 0;
  queue->tail = 0;
  queue->count = 0;
  return pdPASS;
}

inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item,
                             TickType_t) {
  if (queue == nullptr || item == nullptr || queue->count == queue->capacity)
    return 0;
  memcpy(queue->storage + queue->tail * queue->item_size, item,
         queue->item_size);
  queue->tail = (queue->tail + 1) % queue->capacity;
  ++queue->count;
  return pdPASS;
}

inline BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t) {
  if (queue == nullptr || item == nullptr || queue->count == 0) return 0;
  memcpy(item, queue->storage + queue->head * queue->item_size,
         queue->item_size);
  queue->head = (queue->head + 1) % queue->capacity;
  --queue->count;
  return pdPASS;
}

