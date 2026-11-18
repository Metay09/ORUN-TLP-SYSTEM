// KQ05 PC-only NOR verification. This is NOT a Gateway product Flash port.
// A physical 39-page mount remains forbidden until explicit role/cutover.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kq05_segmented_flash_host.h"
#include "custody_store_product_format.h"
#include "observation_store.h"
#include "product_custody_store.h"

namespace k = orun_tlp::kq05_segmented_host;
namespace fmt = orun_tlp::custody_store_product_format;
using orun_tlp::FlashBackend;
using orun_tlp::FlashOpResult;
using orun_tlp::ProductCustodyStore;
constexpr uint64_t kGatewayIdentity = UINT64_C(0x4757415949443031);

namespace {

template <uint32_t Pages>
class FakeNor final : public FlashBackend {
 public:
  uint8_t bytes[Pages * k::kPageSize]{};
  uint32_t starts = 0U;
  uint32_t programs = 0U;
  uint32_t erases = 0U;
  uint32_t polls = 0U;
  bool fail_begin = false;
  bool fail_next = false;
  bool async_next = false;
  bool unreconciled = false;

  FakeNor() { memset(bytes, 0xFF, sizeof(bytes)); }

  bool begin() override {
    ++starts;
    return !fail_begin;
  }
  bool read(uint32_t offset, void* out, size_t len) const override {
    if (out == nullptr || offset >= sizeof(bytes) ||
        len > sizeof(bytes) - offset) return false;
    memcpy(out, bytes + offset, len);
    return true;
  }
  FlashOpResult program(uint32_t offset, const void* data, size_t len) override {
    if (pending_kind_ != 0 || data == nullptr || len == 0U ||
        len > sizeof(pending_payload_) ||
        offset >= sizeof(bytes) || len > sizeof(bytes) - offset)
      return FlashOpResult::kFailed;
    ++programs;
    if (fail_next) {
      fail_next = false;
      return FlashOpResult::kFailed;
    }
    if (async_next) {
      async_next = false;
      pending_kind_ = 1;
      pending_offset_ = offset;
      pending_length_ = len;
      memcpy(pending_payload_, data, len);
      return FlashOpResult::kPending;
    }
    return apply(offset, static_cast<const uint8_t*>(data), len)
               ? FlashOpResult::kDone : FlashOpResult::kFailed;
  }
  FlashOpResult erasePage(uint32_t page) override {
    if (pending_kind_ || page >= Pages) return FlashOpResult::kFailed;
    ++erases;
    if (fail_next) { fail_next = false; return FlashOpResult::kFailed; }
    if (async_next) {
      async_next = false;
      pending_kind_ = 2;
      pending_offset_ = page;
      return FlashOpResult::kPending;
    }
    memset(bytes + page * k::kPageSize, 0xFF, k::kPageSize);
    return FlashOpResult::kDone;
  }
  FlashOpResult pollPending() override {
    ++polls;
    if (!pending_kind_) return FlashOpResult::kFailed;
    const int kind = pending_kind_;
    pending_kind_ = 0;
    if (kind == 2) {
      memset(bytes + pending_offset_ * k::kPageSize,
             0xFF, k::kPageSize);
      return FlashOpResult::kDone;
    }
    return apply(pending_offset_, pending_payload_, pending_length_)
               ? FlashOpResult::kDone : FlashOpResult::kFailed;
  }
  bool hasUnreconciledMutation() const override { return unreconciled; }

 private:
  int pending_kind_ = 0;
  uint32_t pending_offset_ = 0U;
  size_t pending_length_ = 0U;
  uint8_t pending_payload_[256]{};

  bool apply(uint32_t offset, const uint8_t* data, size_t len) {
    for (size_t i = 0U; i < len; ++i)
      if ((bytes[offset + i] & data[i]) != data[i]) return false;
    for (size_t i = 0U; i < len; ++i) bytes[offset + i] &= data[i];
    return true;
  }
};

void put32(uint8_t* p, uint32_t n) {
  p[0] = static_cast<uint8_t>(n >> 24U);
  p[1] = static_cast<uint8_t>(n >> 16U);
  p[2] = static_cast<uint8_t>(n >> 8U);
  p[3] = static_cast<uint8_t>(n);
}

void protectedObject(uint32_t sequence, uint8_t (&out)[100]) {
  memset(out, 0x5AU, sizeof(out));
  // Exact existing TLPv2 PRODUCT_SECURE periodic object framing.
  out[0] = 2U;
  out[1] = 4U;
  out[2] = 2U;
  out[3] = 1U;
  out[4] = 2U;  // custody-request bit
  out[5] = 1U;
  out[6] = 64U;
  out[7] = 0U;
  out[8] = 0x41U;
  out[9] = 0x42U;
  out[10] = 0x43U;
  out[11] = 0x44U;
  out[12] = 0x45U;
  out[13] = 0x46U;
  out[14] = 0x47U;
  out[15] = 0x48U; // Tracker ID (nonzero)
  put32(out + 16U, 1U);  // key epoch (not UINT32_MAX)
  // nonzero incarnation is already part of the synthetic payload
  put32(out + 28U, sequence);
}

void fillPrimaryExactly(FakeNor<k::kPrimaryPages>& product) {
  uint8_t header[fmt::kPageHeaderSize]{};
  uint8_t blob[fmt::kRecordSize]{};
  for (uint32_t page = 0U; page < k::kPrimaryPages; ++page) {
    assert(fmt::encodePageHeader(page + 1U, header));
    memset(header + fmt::kHeaderActiveOffset, 0, 4U);
    memcpy(product.bytes + page * k::kPageSize,
           header, sizeof(header));
    for (uint32_t slot = 0U; slot < fmt::kRecordsPerPage; ++slot) {
      uint8_t object[100]{};
      protectedObject(page * fmt::kRecordsPerPage + slot + 1U, object);
      assert(fmt::encodeRecord(object, sizeof(object), blob));
      memcpy(product.bytes + page * k::kPageSize +
             fmt::kPageHeaderSize + slot * fmt::kRecordSize,
             blob, sizeof(blob));
    }
  }
}

void testMappingAndNoCrossPartitionWrites() {
  FakeNor<k::kPrimaryPages> product;
  FakeNor<k::kSupplementalPages> extra;
  k::SegmentedFlash invalid(product, extra, orun_tlp::kq05_supplemental::Role::kGateway,
                           UINT64_C(0x4757415949443031), false, true);
  assert(!invalid.begin());
  assert(product.starts == 0U && extra.starts == 0U);
  k::SegmentedFlash joined(product, extra, orun_tlp::kq05_supplemental::Role::kGateway,
                           UINT64_C(0x4757415949443031), true, true);
  assert(joined.begin());

  const uint8_t body[4] = {0U, 1U, 2U, 3U};
  assert(joined.program(0U, body, sizeof(body)) == FlashOpResult::kDone);
  assert(joined.program(k::kPrimaryBytes, body, sizeof(body)) ==
         FlashOpResult::kDone);
  assert(product.programs == 1U && extra.programs == 1U);

  uint8_t found[4]{};
  assert(joined.read(k::kPrimaryBytes, found, sizeof(found)));
  assert(memcmp(found, body, sizeof(found)) == 0);
  assert(joined.program(k::kPrimaryBytes - 2U,
                        body, sizeof(body)) == FlashOpResult::kFailed);
  assert(!joined.read(k::kPrimaryBytes - 2U, found, sizeof(found)));
  assert(joined.program(k::kVirtualBytes - 2U,
                        body, sizeof(body)) == FlashOpResult::kFailed);
  assert(joined.program(k::kVirtualBytes, body, 1U) ==
         FlashOpResult::kFailed);
  assert(joined.erasePage(k::kTotalPages) == FlashOpResult::kFailed);
  assert(product.programs == 1U && extra.programs == 1U);

  assert(joined.erasePage(31U) == FlashOpResult::kDone);
  assert(joined.erasePage(32U) == FlashOpResult::kDone);
  assert(product.erases == 1U && extra.erases == 1U);
}

void testPendingAndFailClosed() {
  FakeNor<k::kPrimaryPages> product;
  FakeNor<k::kSupplementalPages> extra;
  k::SegmentedFlash joined(product, extra, orun_tlp::kq05_supplemental::Role::kGateway,
                           UINT64_C(0x4757415949443031), true, true);
  assert(joined.begin());
  const uint8_t body[4] = {0x01U, 0x02U, 0x03U, 0x04U};
  extra.async_next = true;
  assert(joined.program(k::kPrimaryBytes, body, sizeof(body)) ==
         FlashOpResult::kPending);
  assert(joined.pending());
  assert(joined.program(0U, body, sizeof(body)) == FlashOpResult::kFailed);
  uint8_t value[4]{};
  assert(!joined.read(k::kPrimaryBytes, value, sizeof(value)));
  assert(joined.pollPending() == FlashOpResult::kDone);
  assert(!joined.pending());
  assert(joined.read(k::kPrimaryBytes, value, sizeof(value)));
  assert(memcmp(value, body, sizeof(body)) == 0);
  assert(product.polls == 0U && extra.polls == 1U);

  product.fail_next = true;
  assert(joined.program(0U, body, sizeof(body)) == FlashOpResult::kFailed);
  assert(!joined.ready());
  assert(joined.program(k::kPrimaryBytes + 4U, body, sizeof(body)) ==
         FlashOpResult::kFailed);
  assert(!joined.begin()); // no implicit fault reset
  assert(product.erases == 0U && extra.erases == 0U);
}

void testGatewayExistingOwnerSpillsToPage32() {
  FakeNor<k::kPrimaryPages> product;
  FakeNor<k::kSupplementalPages> extra;
  fillPrimaryExactly(product);  // all primary pages are held, unhanded-off
  k::SegmentedFlash joined(product, extra, orun_tlp::kq05_supplemental::Role::kGateway,
                           UINT64_C(0x4757415949443031), true, true);
  ProductCustodyStore store(joined, k::kTotalPages, kGatewayIdentity);
  assert(store.begin() && store.ready());

  uint8_t object[100]{};
  protectedObject(60000U, object);
  assert(store.requestCustody(object, sizeof(object)) ==
         ProductCustodyStore::Admission::kStarted);
  for (int i = 0; i < 16 && store.busy(); ++i) store.poll();
  bool committed = false;
  ProductCustodyStore::Handle handle{};
  assert(store.takeCustodyResult(committed, handle));
  assert(committed && handle.page == 32U && handle.slot == 0U);
  uint8_t recovered[100]{};
  size_t recovered_size = 0U;
  assert(store.readConfirmedHeld(handle, recovered, recovered_size) ==
         ProductCustodyStore::Lookup::kFound);
  assert(recovered_size == sizeof(object));
  assert(memcmp(recovered, object, sizeof(object)) == 0);
  assert(product.programs == 0U && product.erases == 0U);
  assert(extra.programs >= 5U && extra.erases == 0U);

  // Reboot: same exact record remains owned by the supplemental page.
  ProductCustodyStore restarted(joined, k::kTotalPages, kGatewayIdentity);
  assert(restarted.begin() && restarted.ready());
  ProductCustodyStore::Handle duplicate{};
  assert(restarted.requestCustody(object, sizeof(object), &duplicate) ==
         ProductCustodyStore::Admission::kDuplicateHeld);
  assert(duplicate.page == 32U && duplicate.generation == 33U);
  fmt::PageInspection bound{};
  assert(fmt::inspectPageHeader(extra.bytes, fmt::kPageHeaderSize, bound));
  assert(bound.evidence == fmt::PageEvidence::kActive);
  assert(bound.gateway_bound && bound.gateway_id == kGatewayIdentity);
  // The exact same physical bytes are foreign to a different Gateway.
  ProductCustodyStore foreign(joined, k::kTotalPages,
                              kGatewayIdentity + 1U);
  assert(!foreign.begin() && foreign.faulted());
  assert(product.erases == 0U && extra.erases == 0U);
}

void testUnboundSupplementalAndMissingGatewayIdRefused() {
  FakeNor<k::kPrimaryPages> primary;
  FakeNor<k::kSupplementalPages> extra;
  // A valid OCQ2/v3 header written by the former 32-page store (or
  // another operator) must never become a bound supplemental page.
  uint8_t legacy_header[fmt::kPageHeaderSize]{};
  assert(fmt::encodePageHeader(1U, legacy_header));
  memset(legacy_header + fmt::kHeaderActiveOffset, 0U, 4U);
  memcpy(extra.bytes, legacy_header, sizeof(legacy_header));
  k::SegmentedFlash joined(primary, extra,
      orun_tlp::kq05_supplemental::Role::kGateway,
      kGatewayIdentity, true, true);
  ProductCustodyStore unbound(joined, k::kTotalPages, kGatewayIdentity);
  assert(!unbound.begin() && unbound.faulted());
  assert(primary.programs == 0U && extra.programs == 0U);
  assert(primary.erases == 0U && extra.erases == 0U);

  FakeNor<k::kPrimaryPages> blank_primary;
  FakeNor<k::kSupplementalPages> blank_extra;
  k::SegmentedFlash blank_joined(blank_primary, blank_extra,
      orun_tlp::kq05_supplemental::Role::kGateway,
      kGatewayIdentity, true, true);
  ProductCustodyStore missing_id(blank_joined, k::kTotalPages);
  assert(!missing_id.begin() && missing_id.faulted());
  assert(blank_primary.programs == 0U && blank_extra.programs == 0U);
}

void testTornSupplementalPageBindingRefusesReboot() {
  FakeNor<k::kPrimaryPages> primary;
  FakeNor<k::kSupplementalPages> extra;
  uint8_t header[fmt::kPageHeaderSize]{};
  assert(fmt::encodeBoundPageHeader(1U, kGatewayIdentity, header));
  // Crash after body and v4 identity write, but BEFORE separate 32-bit
  // page commit marker. No successful custody record may be inferred.
  memcpy(extra.bytes, header, fmt::kHeaderCommitOffset);
  memcpy(extra.bytes + fmt::kGatewayIdOffset,
         header + fmt::kGatewayIdOffset,
         fmt::kPageHeaderSize - fmt::kGatewayIdOffset);
  k::SegmentedFlash joined(primary, extra,
      orun_tlp::kq05_supplemental::Role::kGateway,
      kGatewayIdentity, true, true);
  ProductCustodyStore store(joined, k::kTotalPages, kGatewayIdentity);
  assert(!store.begin() && store.faulted());
  assert(primary.programs == 0U && extra.programs == 0U);
  assert(primary.erases == 0U && extra.erases == 0U);
}

void testUnknownLegacyAndForeignBlocked() {
  FakeNor<k::kPrimaryPages> product;
  FakeNor<k::kSupplementalPages> extra;
  extra.bytes[0U] = 'H';  // stale legacy HistoryStore / unknown owner
  extra.bytes[1U] = 'I';
  extra.bytes[2U] = 'S';
  extra.bytes[3U] = 'T';
  k::SegmentedFlash joined(product, extra, orun_tlp::kq05_supplemental::Role::kGateway,
                           UINT64_C(0x4757415949443031), true, true);
  ProductCustodyStore store(joined, k::kTotalPages, kGatewayIdentity);
  assert(!store.begin());
  assert(product.programs == 0U && extra.programs == 0U);
  assert(product.erases == 0U && extra.erases == 0U);
  static_assert(orun_tlp::ObservationStore::kMaxPages ==
                k::kTotalPages, "Tracker must support 39 logical pages");
}

}  // namespace

int main() {
  testMappingAndNoCrossPartitionWrites();
  testPendingAndFailClosed();
  testGatewayExistingOwnerSpillsToPage32();
  testUnboundSupplementalAndMissingGatewayIdRefused();
  testTornSupplementalPageBindingRefusesReboot();
  testUnknownLegacyAndForeignBlocked();
  puts("KQ05 128+28KiB noncontiguous segmented NOR + actual OCQ2 page32 append/reboot: PASS (HOST ONLY)");
  puts("NO REAL FLASH, NO LEGACY HISTORY ERASE, NO GATEWAY BATCH ACK, NO DEVICE UPLOAD");
  return 0;
}
