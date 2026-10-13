#include "sf5g_page_header_candidate.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "custody_store_format.h"
#include "observation_store_format.h"

using namespace orun_tlp::sf5g_page_candidate;

namespace {
void put32(uint8_t* p, uint32_t n) {
  p[0] = static_cast<uint8_t>(n >> 24);
  p[1] = static_cast<uint8_t>(n >> 16);
  p[2] = static_cast<uint8_t>(n >> 8);
  p[3] = static_cast<uint8_t>(n);
}
void commit(uint8_t* p, size_t offset) {
  memset(p + offset, 0, 4U);
}
void testHeader() {
  static_assert(kMagic != orun_tlp::custody_store_format::kPageMagic,
                "never reinterpret SF4B legacy flash");
  static_assert(kMagic != orun_tlp::observation_store_format::kPageMagic,
                "never reinterpret SF5C observation flash");
  std::array<uint8_t, kHeaderSize> out{};
  HeaderInspection result;

  out.fill(0x5AU);
  assert(!encodeStagedHeader(0U, out.data(), out.size()));
  assert(!encodeStagedHeader(UINT64_MAX, out.data(), out.size()));
  assert(out[0] == 0x5AU);
  assert(!encodeStagedHeader(1U, out.data(), 4U));
  assert(!inspectHeader(nullptr, kHeaderSize, result));
  assert(!inspectHeader(out.data(), kHeaderSize - 1U, result));

  out.fill(0xFFU);
  assert(inspectHeader(out.data(), out.size(), result));
  assert(result.evidence == HeaderEvidence::kErased);
  assert(encodeStagedHeader(17U, out.data(), out.size()));
  assert(inspectHeader(out.data(), out.size(), result));
  assert(result.evidence == HeaderEvidence::kStaged);
  const auto staged = out;

  for (size_t prefix = 0U; prefix <= kCommitOffset; ++prefix) {
    auto torn = std::array<uint8_t, kHeaderSize>{};
    torn.fill(0xFFU);
    memcpy(torn.data(), staged.data(), prefix);
    assert(inspectHeader(torn.data(), torn.size(), result));
    assert(result.evidence != HeaderEvidence::kPrepared &&
           result.evidence != HeaderEvidence::kActive);
  }

  out = staged;
  out[kCommitOffset] = 0xFEU;
  assert(inspectHeader(out.data(), out.size(), result));
  assert(result.evidence == HeaderEvidence::kPartialCommit);

  out = staged;
  commit(out.data(), kCommitOffset);
  assert(inspectHeader(out.data(), out.size(), result));
  assert(result.evidence == HeaderEvidence::kPrepared);
  assert(result.generation == 17U);

  out[kActiveOffset] = 0xFEU;
  assert(inspectHeader(out.data(), out.size(), result));
  assert(result.evidence == HeaderEvidence::kPartialActivation);

  out = staged;
  commit(out.data(), kCommitOffset);
  commit(out.data(), kActiveOffset);
  assert(inspectHeader(out.data(), out.size(), result));
  assert(result.evidence == HeaderEvidence::kActive && result.generation == 17U);

  // Any one-bit corruption in the static 28-byte header or its 4-byte
  // CRC must make it unusable as an active or prepared page.
  for (size_t at = 0U; at < kCommitOffset; ++at) {
    for (unsigned bit = 0U; bit < 8U; ++bit) {
      auto damaged = out;
      damaged[at] ^= static_cast<uint8_t>(1U << bit);
      assert(inspectHeader(damaged.data(), damaged.size(), result));
      assert(result.evidence != HeaderEvidence::kActive &&
             result.evidence != HeaderEvidence::kPrepared);
    }
  }
  auto damaged = out;
  damaged[kReservedOffset] = 0U;
  assert(inspectHeader(damaged.data(), damaged.size(), result));
  assert(result.evidence == HeaderEvidence::kCorrupt);

  // Page metadata cannot silently mount foreign/legacy stores.
  for (uint32_t magic : {orun_tlp::custody_store_format::kPageMagic,
                          orun_tlp::observation_store_format::kPageMagic,
                          uint32_t(0xABCD1234U)}) {
    damaged = out;
    put32(damaged.data(), magic);
    assert(inspectHeader(damaged.data(), damaged.size(), result));
    assert(result.evidence == HeaderEvidence::kForeign);
  }
}

void testIntent() {
  static_assert(kIntentSize == 24U, "intent layout changed");
  std::array<uint8_t, kIntentSize> out{};
  out.fill(0xB6U);
  IntentInspection result;
  assert(!encodeStagedIntent(UINT16_MAX, 5U, out.data(), out.size()));
  assert(!encodeStagedIntent(7U, 0U, out.data(), out.size()));
  assert(!encodeStagedIntent(7U, UINT64_MAX, out.data(), out.size()));
  assert(out[0] == 0xB6U);
  assert(!inspectIntent(nullptr, kIntentSize, result));

  out.fill(0xFFU);
  assert(inspectIntent(out.data(), out.size(), result));
  assert(result.evidence == IntentEvidence::kErased);
  assert(encodeStagedIntent(7U, 18U, out.data(), out.size()));
  assert(inspectIntent(out.data(), out.size(), result));
  assert(result.evidence == IntentEvidence::kStaged);
  const auto staged = out;

  for (size_t prefix = 0; prefix <= kIntentCommitOffset; ++prefix) {
    auto torn = std::array<uint8_t, kIntentSize>{};
    torn.fill(0xFFU);
    memcpy(torn.data(), staged.data(), prefix);
    assert(inspectIntent(torn.data(), torn.size(), result));
    assert(result.evidence != IntentEvidence::kCommitted &&
           result.evidence != IntentEvidence::kCompleted);
  }

  out[kIntentCommitOffset] = 0xFEU;
  assert(inspectIntent(out.data(), out.size(), result));
  assert(result.evidence == IntentEvidence::kPartialCommit);
  out = staged;
  commit(out.data(), kIntentCommitOffset);
  assert(inspectIntent(out.data(), out.size(), result));
  assert(result.evidence == IntentEvidence::kCommitted);
  assert(result.target_page == 7U && result.target_generation == 18U);

  out[kIntentDoneOffset] = 0xFEU;
  assert(inspectIntent(out.data(), out.size(), result));
  assert(result.evidence == IntentEvidence::kPartialCompletion);
  out = staged;
  commit(out.data(), kIntentCommitOffset);
  commit(out.data(), kIntentDoneOffset);
  assert(inspectIntent(out.data(), out.size(), result));
  assert(result.evidence == IntentEvidence::kCompleted);
  assert(result.target_page == 7U && result.target_generation == 18U);

  for (size_t at = 0U; at < kIntentCommitOffset; ++at) {
    for (unsigned bit = 0U; bit < 8U; ++bit) {
      auto damaged = out;
      damaged[at] ^= static_cast<uint8_t>(1U << bit);
      assert(inspectIntent(damaged.data(), damaged.size(), result));
      assert(result.evidence != IntentEvidence::kCommitted &&
             result.evidence != IntentEvidence::kCompleted);
    }
  }
}
}  // namespace

int main() {
  testHeader();
  testIntent();
  puts("SF5G page header/intent candidate: staged/commit/foreign/torn guards: PASS");
}
