#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// M7P6E TEST-ONLY candidate security-contract fixture.
//
// These are fixed public test values. They are deliberately not provisioned
// credentials and must never be sourced from SecurityStore. The same header is
// consumed by the host byte-layout test and the RAK4630 coexistence probe so
// KDF-info/nonce serialization cannot drift between them.
//
// M7P6D remains a candidate contract, not a frozen wire format. This fixture
// proves only the exact candidate bytes recorded in docs/milestones/M7P6E.md.
namespace orun_tlp {
namespace m7p6e_test {

static constexpr uint8_t kDirectionD2A = 0x01U;
static constexpr uint8_t kDirectionA2D = 0x02U;
static constexpr uint32_t kKeyEpoch = 0x01020304UL;
static constexpr uint64_t kTxCounter = UINT64_C(0x1122334455667788);

static constexpr size_t kRootSize = 32U;
static constexpr size_t kCredentialIdSize = 16U;
static constexpr size_t kLabelSize = 16U;
static constexpr size_t kInfoSize = 21U;
static constexpr size_t kTrafficKeySize = 16U;
static constexpr size_t kNonceSize = 13U;
static constexpr size_t kAadSize = 14U;
static constexpr size_t kPlaintextSize = 23U;
static constexpr size_t kTagSize = 8U;

static const uint8_t kRoot[kRootSize] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

static const uint8_t kCredentialId[kCredentialIdSize] = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
};

static const uint8_t kLabel[kLabelSize] = {
    'O', 'R', 'U', 'N', '-', 'T', 'L', 'P',
    '-', 'V', '2', '-', 'A', 'E', 'A', 'D',
};

static const uint8_t kProbeAad[kAadSize] = {
    'O', 'R', 'U', 'N', '-', 'M', '7', 'P',
    '6', 'E', '-', 'K', 'A', 'T',
};

static const uint8_t kPlaintext[kPlaintextSize] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
};

// Reference outputs generated out-of-target with Python cryptography 46.0.4.
// They must be independently regenerated during final security review.
static const uint8_t kExpectedD2AKey[kTrafficKeySize] = {
    0xB4, 0xDB, 0x25, 0xA9, 0x9B, 0xAD, 0xE8, 0x34,
    0xD0, 0x06, 0xC0, 0x99, 0x2D, 0x6D, 0xBE, 0x1A,
};

static const uint8_t kExpectedA2DKey[kTrafficKeySize] = {
    0xDE, 0xA7, 0x6F, 0x45, 0xA7, 0xAB, 0xC0, 0x42,
    0x33, 0x84, 0x83, 0x35, 0xB1, 0x6A, 0x3E, 0xE1,
};

static const uint8_t kExpectedCiphertext[kPlaintextSize] = {
    0x60, 0xFF, 0x0E, 0xC3, 0xE2, 0x11, 0xAE, 0x14,
    0x3C, 0xA6, 0xC1, 0x15, 0xC1, 0x50, 0xBD, 0x6F,
    0x05, 0xDA, 0x47, 0xE4, 0x16, 0x85, 0x6B,
};

static const uint8_t kExpectedTag[kTagSize] = {
    0xBD, 0x9B, 0x12, 0x04, 0x40, 0x81, 0x69, 0x7B,
};

inline void writeBe32(uint32_t value, uint8_t* out) {
  out[0] = static_cast<uint8_t>(value >> 24);
  out[1] = static_cast<uint8_t>(value >> 16);
  out[2] = static_cast<uint8_t>(value >> 8);
  out[3] = static_cast<uint8_t>(value);
}

inline void writeBe64(uint64_t value, uint8_t* out) {
  for (uint8_t i = 0; i < 8U; ++i) {
    out[i] = static_cast<uint8_t>(value >> (56U - 8U * i));
  }
}

inline void buildInfo(uint8_t direction, uint8_t out[kInfoSize]) {
  memcpy(out, kLabel, kLabelSize);
  out[kLabelSize] = direction;
  writeBe32(kKeyEpoch, out + kLabelSize + 1U);
}

inline void buildNonce(uint8_t direction, uint64_t counter,
                       uint8_t out[kNonceSize]) {
  writeBe32(kKeyEpoch, out);
  out[4] = direction;
  writeBe64(counter, out + 5U);
}

}  // namespace m7p6e_test
}  // namespace orun_tlp
