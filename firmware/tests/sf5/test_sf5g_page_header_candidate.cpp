#include "sf5g_page_header_candidate.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <initializer_list>

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
void put16(uint8_t* p, uint16_t n) {
  p[0] = static_cast<uint8_t>(n >> 8);
  p[1] = static_cast<uint8_t>(n);
}
void put64(uint8_t* p, uint64_t n) {
  for (unsigned i = 0U; i < 8U; ++i)
    p[i] = static_cast<uint8_t>(n >> (56U - 8U * i));
}
using Header = std::array<uint8_t, kHeaderSize>;
using Intent = std::array<uint8_t, kIntentSize>;
void resealHeader(Header& raw) {
  put32(raw.data() + kStaticCrcOffset,
        record::crc32(raw.data(), kStaticCrcOffset));
}
void resealIntent(Intent& raw) {
  put32(raw.data() + kIntentCrcOffset,
        record::crc32(raw.data(), kIntentCrcOffset));
}
void rejectHeader(const Header& raw) {
  // A valid CRC excludes the easy false positive where the CRC, not
  // semantic validation, is solely responsible for rejection.
  assert(record::crc32(raw.data(), kStaticCrcOffset) ==
         (uint32_t(raw[kStaticCrcOffset]) << 24 |
          uint32_t(raw[kStaticCrcOffset + 1U]) << 16 |
          uint32_t(raw[kStaticCrcOffset + 2U]) << 8 |
          uint32_t(raw[kStaticCrcOffset + 3U])));
  HeaderInspection state;
  assert(inspectHeader(raw.data(), raw.size(), state));
  assert(state.evidence == HeaderEvidence::kCorrupt);
  assert(state.generation == 0U);
}
void rejectIntent(const Intent& raw) {
  assert(record::crc32(raw.data(), kIntentCrcOffset) ==
         (uint32_t(raw[kIntentCrcOffset]) << 24 |
          uint32_t(raw[kIntentCrcOffset + 1U]) << 16 |
          uint32_t(raw[kIntentCrcOffset + 2U]) << 8 |
          uint32_t(raw[kIntentCrcOffset + 3U])));
  IntentInspection state;
  assert(inspectIntent(raw.data(), raw.size(), state));
  assert(state.evidence == IntentEvidence::kCorrupt);
  assert(state.target_page == UINT16_MAX && state.target_generation == 0U);
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

  // A partial OCP3 magic is intentionally FOREIGN, not automatically
  // reclaimable or even provably our page. This is a documented, safe but
  // currently liveness-limiting preflight choice.
  for (size_t prefix = 1U; prefix < 4U; ++prefix) {
    Header torn{};
    torn.fill(0xFFU);
    memcpy(torn.data(), staged.data(), prefix);
    assert(inspectHeader(torn.data(), torn.size(), result));
    assert(result.evidence == HeaderEvidence::kForeign);
  }
  auto partialMagic = staged;
  partialMagic[0] |= 0x80U;  // NOR partial program: clear fewer bits
  assert(inspectHeader(partialMagic.data(), partialMagic.size(), result));
  assert(result.evidence == HeaderEvidence::kForeign);
  partialMagic = staged;
  partialMagic[1] = 0xFFU;
  commit(partialMagic.data(), kCommitOffset);
  assert(inspectHeader(partialMagic.data(), partialMagic.size(), result));
  assert(result.evidence == HeaderEvidence::kForeign);

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
  assert(result.generation == 17U);  // diagnostic-only, never mount authority

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
  assert(result.target_page == 7U && result.target_generation == 18U);
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

// Test semantic contract checks with CRC32 RESEALED after every mutation.
// Without this, deleting version, inverse, generation and geometry guards
// would still PASS the ordinary bit-flip checks (the independent audit's
// MEDIUM-1 finding).
void testSemanticHeaderGuards() {
  Header staged{};
  assert(encodeStagedHeader(17U, staged.data(), staged.size()));
  Header prepared = staged;
  commit(prepared.data(), kCommitOffset);
  Header active = prepared;
  commit(active.data(), kActiveOffset);
  struct GeometryCase { size_t offset; uint16_t wrong; };
  const GeometryCase geometry[] = {
      {16U, 63U},  // header size
      {18U, 92U},  // legacy record size
      {20U, 33U},  // records per page
      {22U, 73U},  // legacy object max
      {24U, 23U},  // intent slot size
      {26U, 4U},   // intent slot count
  };
  // BOTH prepared and active headers must reject unsupported metadata.
  for (const auto& valid : {prepared, active}) {
    for (uint16_t version : {uint16_t(2U), uint16_t(4U)}) {
      Header bad = valid;
      put16(bad.data() + 4U, version);
      put16(bad.data() + 6U, static_cast<uint16_t>(~version));
      resealHeader(bad);
      rejectHeader(bad);
    }
    Header badInverse = valid;
    put16(badInverse.data() + 6U, uint16_t(0x1234U));
    resealHeader(badInverse);
    rejectHeader(badInverse);

    for (uint64_t gen : {uint64_t(0U), uint64_t(UINT64_MAX)}) {
      Header bad = valid;
      put64(bad.data() + 8U, gen);
      resealHeader(bad);
      rejectHeader(bad);
    }
    for (const auto& entry : geometry) {
      Header bad = valid;
      put16(bad.data() + entry.offset, entry.wrong);
      resealHeader(bad);
      rejectHeader(bad);
    }
  }
}

void testSemanticIntentGuards() {
  Intent staged{};
  assert(encodeStagedIntent(7U, 18U, staged.data(), staged.size()));
  Intent committed = staged;
  commit(committed.data(), kIntentCommitOffset);
  Intent completed = committed;
  commit(completed.data(), kIntentDoneOffset);

  // Encoding never emits an intent outside the 128 KiB product region;
  // rejected writes leave output bytes unchanged.
  Intent untouched{};
  untouched.fill(0xA5U);
  for (uint16_t page : {uint16_t(32U), uint16_t(7000U), uint16_t(UINT16_MAX)}) {
    assert(!encodeStagedIntent(page, 18U,
                               untouched.data(), untouched.size()));
    assert(untouched[0] == 0xA5U);
  }

  // CRC is recalculated for all the invalid fields, so removing semantic
  // guards is detectable even though CRC checking itself still works.
  for (const auto& valid : {committed, completed}) {
    for (uint16_t page : {kMaxRegionPages, uint16_t(7000U), uint16_t(UINT16_MAX)}) {
      Intent bad = valid;
      put16(bad.data(), page);
      resealIntent(bad);
      rejectIntent(bad);
    }
    Intent reserved = valid;
    put16(reserved.data() + 2U, 1U);
    resealIntent(reserved);
    rejectIntent(reserved);

    for (uint64_t gen : {uint64_t(0U), uint64_t(UINT64_MAX)}) {
      Intent bad = valid;
      put64(bad.data() + 4U, gen);
      resealIntent(bad);
      rejectIntent(bad);
    }
  }
}
}  // namespace

int main() {
  testHeader();
  testIntent();
  testSemanticHeaderGuards();
  testSemanticIntentGuards();
  puts("SF5G page header/intent candidate: staged/commit/foreign/torn guards: PASS");
}
