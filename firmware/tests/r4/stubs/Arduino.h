#pragma once

#include <stdint.h>

inline uint32_t fake_reset_reason = 0;
inline uint32_t readResetReason() { return fake_reset_reason; }
