// HOST ONLY. Simulated nRF 4 KiB NOR pages, never a real RAK.
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kq05_gateway_receipt_journal.h"

namespace fmt = orun_tlp::kq05_receipt_journal;
using Store = orun_tlp::Kq05GatewayReceiptJournal;
using orun_tlp::FlashBackend;
using orun_tlp::FlashOpResult;

namespace {

constexpr uint64_t kGateway = UINT64_C(0x4741544557415931);

struct Nor final : FlashBackend {
  uint8_t bytes[fmt::kHistoryBytes]{};
  uint32_t writes = 0U;
  uint32_t erases = 0U;
  uint32_t fail_call = 0U;
  uint32_t partial_bytes = 0U;
  uint32_t pending_call = 0U;
  bool fail_reads = false;
  Nor() { memset(bytes, 0xFF, sizeof(bytes)); }
  bool begin() override { return true; }
  bool read(uint32_t offset, void* data, size_t length) const override {
    if (fail_reads || data == nullptr ||
        offset > sizeof(bytes) || length > sizeof(bytes) - offset)
      return false;
    memcpy(data, bytes + offset, length);
    return true;
  }
  FlashOpResult program(
      uint32_t offset, const void* data, size_t length) override {
    ++writes;
    if (writes == pending_call) return FlashOpResult::kPending;
    if (data == nullptr || offset > sizeof(bytes) ||
        length > sizeof(bytes) - offset)
      return FlashOpResult::kFailed;
    const auto* value = static_cast<const uint8_t*>(data);
    const bool partial = writes == fail_call;
    const size_t limit = partial && partial_bytes < length
        ? partial_bytes : length;
    for (size_t i = 0; i < limit; ++i) {
      if ((bytes[offset + i] & value[i]) != value[i])
        return FlashOpResult::kFailed;
      bytes[offset + i] &= value[i];
    }
    return partial ? FlashOpResult::kFailed : FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t) override {
    ++erases;
    return FlashOpResult::kFailed;
  }
};

void offerFor(uint64_t row, uint8_t (&offer)[96]) {
  memset(offer, 0xA5, sizeof(offer));
  offer[0] = 0x91U;
  offer[1] = 1U;
  offer[2] = 0x91U;
  const uint8_t gw[8] = {'G','A','T','E','W','A','Y','1'};
  memcpy(offer + 3U, gw, sizeof(gw));
  for (unsigned n = 0U; n < 8U; ++n)
    offer[11U + n] = uint8_t(row >> (8U * (7U - n)));
  offer[19] = 44U;
  offer[20] = 2U;
  offer[21] = 9U;
}

void assertReservedUntouched(const Nor& nor) {
  for (size_t n = 2U * fmt::kPageSize; n < sizeof(nor.bytes); ++n)
    assert(nor.bytes[n] == 0xFFU);
  assert(nor.erases == 0U);
}

void testOneCopyAndFull() {
  Nor nor;
  Store q(nor, kGateway);
  uint8_t first[96]{};
  offerFor(1U, first);
  assert(q.mountReadOnly() == Store::Mount::kEmpty);
  assert(q.putVerified(first, sizeof(first), true) == Store::Put::kFault);
  assert(q.initializeErased());
  assert(q.ready());
  assert(q.putVerified(first, sizeof(first), false) == Store::Put::kRejected);
  assert(q.putVerified(first, sizeof(first), true) == Store::Put::kStored);
  assert(q.putVerified(first, sizeof(first), true) == Store::Put::kDuplicate);
  uint8_t conflict[96]{};
  memcpy(conflict, first, sizeof(conflict));
  conflict[30] ^= 1U;
  assert(q.putVerified(conflict, sizeof(conflict), true) ==
         Store::Put::kConflict);
  uint8_t readback[fmt::kOfferMax]{};
  size_t readback_len = 0U;
  assert(q.read(0U, readback, readback_len));
  assert(readback_len == sizeof(first) &&
         memcmp(first, readback, sizeof(first)) == 0);
  assert(q.committed() == 1U && q.occupied() == 1U);

  Store restarted(nor, kGateway);
  assert(restarted.mountReadOnly() == Store::Mount::kReady);
  assert(restarted.putVerified(first, sizeof(first), true) ==
         Store::Put::kDuplicate);
  for (uint64_t row = 2U; row <= fmt::kCapacity; ++row) {
    uint8_t offer[96]{};
    offerFor(row, offer);
    assert(restarted.putVerified(offer, sizeof(offer), true) ==
           Store::Put::kStored);
  }
  assert(restarted.committed() == fmt::kCapacity);
  assert(restarted.occupied() == fmt::kCapacity);
  uint8_t overflow[96]{};
  offerFor(57U, overflow);
  assert(restarted.putVerified(overflow, sizeof(overflow), true) ==
         Store::Put::kFull);
  assert(restarted.putVerified(first, sizeof(first), true) ==
         Store::Put::kDuplicate);
  Store afterFull(nor, kGateway);
  assert(afterFull.mountReadOnly() == Store::Mount::kReady);
  assert(afterFull.committed() == fmt::kCapacity);
  assert(afterFull.read(fmt::kCapacity - 1U, readback, readback_len));
  assert(memcmp(readback + 11U, overflow + 11U, 8U) != 0);
  assertReservedUntouched(nor);
}

void testTornRecordRecovery() {
  Nor nor;
  Store q(nor, kGateway);
  assert(q.mountReadOnly() == Store::Mount::kEmpty);
  assert(q.initializeErased());
  uint8_t one[96]{};
  offerFor(1U, one);
  assert(q.putVerified(one, sizeof(one), true) == Store::Put::kStored);

  uint8_t torn[96]{};
  offerFor(2U, torn);
  nor.fail_call = nor.writes + 1U;
  nor.partial_bytes = 37U;
  assert(q.putVerified(torn, sizeof(torn), true) == Store::Put::kFault);
  assert(q.state() == Store::Mount::kFault);
  nor.fail_call = 0U;

  Store recovered(nor, kGateway);
  assert(recovered.mountReadOnly() == Store::Mount::kReady);
  assert(recovered.committed() == 1U && recovered.occupied() == 2U);
  uint8_t three[96]{};
  offerFor(3U, three);
  assert(recovered.putVerified(three, sizeof(three), true) ==
         Store::Put::kStored);
  assert(recovered.committed() == 2U && recovered.occupied() == 3U);
  uint8_t result[fmt::kOfferMax]{};
  size_t size = 0U;
  assert(recovered.read(1U, result, size) &&
         size == sizeof(three) &&
         memcmp(result, three, sizeof(three)) == 0);
  assertReservedUntouched(nor);
}

void testTornCommitRecovery() {
  Nor nor;
  Store q(nor, kGateway);
  assert(q.mountReadOnly() == Store::Mount::kEmpty);
  assert(q.initializeErased());
  uint8_t offer[96]{};
  offerFor(1U, offer);
  nor.fail_call = nor.writes + 2U;  // body committed, marker torn
  nor.partial_bytes = 2U;
  assert(q.putVerified(offer, sizeof(offer), true) == Store::Put::kFault);
  nor.fail_call = 0U;
  Store restarted(nor, kGateway);
  assert(restarted.mountReadOnly() == Store::Mount::kReady);
  assert(restarted.committed() == 0U && restarted.occupied() == 1U);
  assert(restarted.putVerified(offer, sizeof(offer), true) ==
         Store::Put::kStored);
  assert(restarted.committed() == 1U);
  assertReservedUntouched(nor);
}

void testHeaderAndForeignFailures() {
  Nor nor;
  Store old(nor, kGateway);
  assert(old.mountReadOnly() == Store::Mount::kEmpty);
  nor.fail_call = nor.writes + 1U;
  nor.partial_bytes = 8U;
  assert(!old.initializeErased());
  nor.fail_call = 0U;
  Store blocked(nor, kGateway);
  assert(blocked.mountReadOnly() == Store::Mount::kFault);
  assertReservedUntouched(nor);

  Nor another;
  Store prepared(another, kGateway);
  assert(prepared.mountReadOnly() == Store::Mount::kEmpty);
  assert(prepared.initializeErased());
  Store foreign(another, kGateway + 1U);
  assert(foreign.mountReadOnly() == Store::Mount::kForeign);
  another.bytes[4] ^= 0x01U;
  Store corrupt(another, kGateway);
  assert(corrupt.mountReadOnly() == Store::Mount::kFault);

  Nor legacy;
  legacy.bytes[0] = 0x12U;  // Old History / unknown magic: never format.
  Store noMigration(legacy, kGateway);
  assert(noMigration.mountReadOnly() == Store::Mount::kFault);
  assertReservedUntouched(legacy);
}

void testCorruptEntryAndPending() {
  Nor nor;
  Store q(nor, kGateway);
  assert(q.mountReadOnly() == Store::Mount::kEmpty);
  assert(q.initializeErased());
  uint8_t one[96]{};
  offerFor(1U, one);
  assert(q.putVerified(one, sizeof(one), true) == Store::Put::kStored);
  nor.bytes[fmt::kPageHeaderSize + 14U] ^= 0x01U;
  Store corrupt(nor, kGateway);
  assert(corrupt.mountReadOnly() == Store::Mount::kFault);

  Nor pending;
  Store notAsync(pending, kGateway);
  assert(notAsync.mountReadOnly() == Store::Mount::kEmpty);
  assert(notAsync.initializeErased());
  pending.pending_call = pending.writes + 1U;
  assert(notAsync.putVerified(one, sizeof(one), true) == Store::Put::kFault);
  assert(notAsync.state() == Store::Mount::kFault);
  assertReservedUntouched(pending);
}

}  // namespace

int main() {
  testOneCopyAndFull();
  testTornRecordRecovery();
  testTornCommitRecovery();
  testHeaderAndForeignFailures();
  testCorruptEntryAndPending();
  puts("KQ05 8KiB receipt Journal owner duplicate/full/reboot/torn/foreign: PASS (HOST FAKE NOR ONLY)");
  puts("NO REAL FLASH, NO ERASE, NO STAGED, NO TRACKER RELEASE");
  return 0;
}
