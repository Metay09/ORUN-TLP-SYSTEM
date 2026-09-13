#pragma once

#include <stddef.h>
#include <stdint.h>
#include <assert.h>

using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = uint32_t;

constexpr BaseType_t pdPASS = 1;
constexpr BaseType_t pdTRUE = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
inline unsigned fake_task = 1;
inline unsigned critical_depth = 0;
#define configASSERT(value) assert(value)

#define taskENTER_CRITICAL() (++critical_depth)
#define taskEXIT_CRITICAL() do { assert(critical_depth); --critical_depth; } while (0)
