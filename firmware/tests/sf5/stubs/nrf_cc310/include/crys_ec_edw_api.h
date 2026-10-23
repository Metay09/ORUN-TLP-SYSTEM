#pragma once
#include <stddef.h>
#include <stdint.h>
#include "crys_error.h"
// HOST-ONLY header shape: the actual nRF CC310 headers are used by target.
#define CRYS_ECEDW_SIGNATURE_BYTES (64U)
#define CRYS_ECEDW_MOD_SIZE_IN_BYTES (32U)
typedef struct CRYS_ECEDW_TempBuff_t {
  uint32_t scratch[128];
} CRYS_ECEDW_TempBuff_t;
#ifdef __cplusplus
extern "C" {
#endif
CRYSError_t CRYS_ECEDW_Verify(
    const uint8_t* signature, size_t signature_size,
    const uint8_t* public_key, size_t public_key_size,
    uint8_t* message, size_t message_size,
    CRYS_ECEDW_TempBuff_t* context);
#ifdef __cplusplus
}
#endif
