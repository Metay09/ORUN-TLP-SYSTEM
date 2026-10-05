#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <array>

#include "history_delivery_coordinator.h"
#include "journal_format.h"
#include "storage_config.h"
#include "tlp_position_packet.h"

using namespace orun_tlp;
using namespace orun_tlp::journal_format;
using namespace orun_tlp::storage_config;

namespace {

constexpr uint64_t kDevice = UINT64_C(0x123456789ABCDEF0);
constexpr uint64_t kIncarnation = UINT64_C(0x1122334455667788);

class TestIncarnationSource final : public HistoryIncarnationSource {
 public:
  bool generate(uint64_t& incarnation) override {
    incarnation = next;
    return true;
  }

  uint64_t next = kIncarnation;
};

class TestFlash final : public FlashBackend {
 public:
  TestFlash() { bytes.fill(0xFF); }

  bool begin() override { return true; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (read_fail_countdown == 0) {
      read_fail_countdown = -1;
      ++read_failures;
      return false;
    }
    if (read_fail_countdown > 0) --read_fail_countdown;
    if (data == nullptr || offset > bytes.size() ||
        size > bytes.size() - offset)
      return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  void failOneReadAfter(uint32_t successful_reads) {
    read_fail_countdown = static_cast<int64_t>(successful_reads);
  }

  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    if (data == nullptr || size == 0 ||
        (offset & 3U) != 0 || (size & 3U) != 0 ||
        offset > bytes.size() || size > bytes.size() - offset)
      return FlashOpResult::kFailed;

    const auto* source = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
      if ((bytes[offset + i] & source[i]) != source[i])
        return FlashOpResult::kFailed;
    }
    for (size_t i = 0; i < size; ++i)
      bytes[offset + i] &= source[i];
    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    if (page >= kPageCount) return FlashOpResult::kFailed;
    const size_t first = size_t(page) * kPageSize;
    memset(bytes.data() + first, 0xFF, kPageSize);
    return FlashOpResult::kDone;
  }

  std::array<uint8_t, kRegionSize> bytes{};
  mutable int64_t read_fail_countdown = -1;
  mutable uint32_t read_failures = 0;
};

void settle(HistoryStore& store) {
  for (unsigned pass = 0; pass < 200 && store.busy(); ++pass)
    store.poll();
  assert(!store.busy());
}

void start(HistoryStore& store) {
  assert(store.begin(kDevice));
  settle(store);
  if (!store.canAppend()) {
    assert(!store.prepareAppend());
    settle(store);
  }
  assert(store.ready());
  assert(store.canAppend());
}

HistoryStore::Record allocate(HistoryStore& store, int32_t latitude) {
  uint32_t sequence = 0;
  HistoryStore::Record record{};
  if (!store.nextSequence(sequence, record.identity)) {
    assert(store.busy());
    settle(store);
    assert(store.nextSequence(sequence, record.identity));
  }

  tlp::PositionPacket packet{};
  packet.source_device_id = kDevice;
  packet.sequence_number = sequence;
  packet.gnss_utc_epoch_seconds = 0x11223344U + sequence;
  packet.latitude_e7 = latitude;
  packet.longitude_e7 = -290000000;
  packet.altitude_mm = 1234;
  packet.hdop_x100 = 150;
  packet.satellites = 8;
  packet.flags = tlp::kPositionFlagValidFix |
                 tlp::kPositionFlagValidUtcTime |
                 tlp::kPositionFlag3dFix;
  assert(tlp::serializePositionPacket(
      packet, record.packet, sizeof(record.packet)));
  return record;
}

void append(HistoryStore& store, const HistoryStore::Record& record) {
  assert(store.append(record.packet, record.identity));
  settle(store);
  bool success = false;
  assert(store.takeAppendResult(success));
  assert(success);
}

tlp::BackendDurableReceiptPlaintext receipt(
    const uint64_t* ids, uint8_t count) {
  tlp::BackendDurableReceiptPlaintext out{};
  out.count = count;
  for (uint8_t i = 0; i < count; ++i)
    out.history_record_identities[i] = ids[i];
  return out;
}

HistoryDeliveryCoordinator::ApplyResult apply(
    HistoryDeliveryCoordinator& coordinator,
    const tlp::BackendDurableReceiptPlaintext& value,
    uint64_t authenticated_history_incarnation = kIncarnation) {
  return coordinator.applyReplayAcceptedBackendDurableReceipt(
      value, authenticated_history_incarnation);
}

void selectiveReceiptWaitsForGapThenDrains() {
  TestFlash flash;
  TestIncarnationSource incarnation;
  HistoryStore store(flash, &incarnation);
  start(store);

  const auto first = allocate(store, 410000001);
  const auto second = allocate(store, 410000002);
  const auto third = allocate(store, 410000003);
  append(store, first);
  append(store, second);
  append(store, third);

  HistoryDeliveryCoordinator coordinator(store);

  const uint64_t second_only[] = {second.identity};
  assert(apply(coordinator, 
             receipt(second_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.acknowledgedThrough() == 0);
  assert(coordinator.selectiveAcknowledgementCount() == 1);

  const uint64_t first_only[] = {first.identity};
  assert(apply(coordinator, 
             receipt(first_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.acknowledgedThrough() == second.identity);
  assert(coordinator.selectiveAcknowledgementCount() == 0);

  const uint64_t first_second[] = {first.identity, second.identity};
  assert(apply(coordinator, 
             receipt(first_second, 2)) ==
         HistoryDeliveryCoordinator::ApplyResult::kDuplicateOnly);
  assert(coordinator.acknowledgedThrough() == second.identity);
}

void numericTicketGapNeedsExplicitLaterIdentity() {
  TestFlash flash;
  TestIncarnationSource incarnation;
  HistoryStore store(flash, &incarnation);
  start(store);

  const auto first = allocate(store, 420000001);
  const auto skipped_one = allocate(store, 420000002);
  const auto skipped_two = allocate(store, 420000003);
  const auto fourth = allocate(store, 420000004);
  (void)skipped_one;
  (void)skipped_two;
  append(store, first);
  append(store, fourth);

  HistoryDeliveryCoordinator coordinator(store);

  const uint64_t fourth_only[] = {fourth.identity};
  assert(apply(coordinator, 
             receipt(fourth_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.acknowledgedThrough() == 0);

  const uint64_t first_only[] = {first.identity};
  assert(apply(coordinator, 
             receipt(first_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);

  // There were numeric ticket gaps, but the next actual retained record was
  // explicitly acknowledged, so contiguous actual-record progress reaches it.
  assert(coordinator.acknowledgedThrough() == fourth.identity);
  assert(coordinator.selectiveAcknowledgementCount() == 0);
}

void unknownIdentityRejectsAtomically() {
  TestFlash flash;
  TestIncarnationSource incarnation;
  HistoryStore store(flash, &incarnation);
  start(store);

  const auto first = allocate(store, 430000001);
  const auto second = allocate(store, 430000002);
  append(store, first);
  append(store, second);

  HistoryDeliveryCoordinator coordinator(store);

  const uint64_t second_only[] = {second.identity};
  assert(apply(coordinator, 
             receipt(second_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.selectiveAcknowledgementCount() == 1);

  const uint64_t bad[] = {first.identity, UINT64_C(999999)};
  assert(apply(coordinator, 
             receipt(bad, 2)) ==
         HistoryDeliveryCoordinator::ApplyResult::kUnknownIdentity);

  // The valid first identity from the same rejected batch must not partially
  // advance delivery or consume the previously selected second identity.
  assert(coordinator.acknowledgedThrough() == 0);
  assert(coordinator.selectiveAcknowledgementCount() == 1);

  const uint64_t first_only[] = {first.identity};
  assert(apply(coordinator, 
             receipt(first_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.acknowledgedThrough() == second.identity);
}

void selectiveSetIsBoundedAndFailureIsAtomic() {
  TestFlash flash;
  TestIncarnationSource incarnation;
  HistoryStore store(flash, &incarnation);
  start(store);

  HistoryStore::Record records[14]{};
  for (size_t i = 0; i < 14; ++i) {
    records[i] = allocate(store, 440000000 + static_cast<int32_t>(i));
    append(store, records[i]);
  }

  HistoryDeliveryCoordinator coordinator(store);

  uint64_t batch_one[tlp::kHistoryReceiptMaxIdentities]{};
  uint64_t batch_two[tlp::kHistoryReceiptMaxIdentities]{};
  for (size_t i = 0; i < tlp::kHistoryReceiptMaxIdentities; ++i) {
    batch_one[i] = records[i + 1].identity;
    batch_two[i] = records[i + 1 + tlp::kHistoryReceiptMaxIdentities].identity;
  }

  assert(apply(coordinator, 
             receipt(batch_one, tlp::kHistoryReceiptMaxIdentities)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(apply(coordinator, 
             receipt(batch_two, tlp::kHistoryReceiptMaxIdentities)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.selectiveAcknowledgementCount() ==
         HistoryDeliveryCoordinator::kMaxSelectiveAcknowledgements);
  assert(coordinator.acknowledgedThrough() == 0);

  const uint64_t fourteenth[] = {records[13].identity};
  assert(apply(coordinator, 
             receipt(fourteenth, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kSelectiveSetFull);
  assert(coordinator.selectiveAcknowledgementCount() ==
         HistoryDeliveryCoordinator::kMaxSelectiveAcknowledgements);
  assert(coordinator.acknowledgedThrough() == 0);

  const uint64_t first_only[] = {records[0].identity};
  assert(apply(coordinator, 
             receipt(first_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.acknowledgedThrough() == records[12].identity);
  assert(coordinator.selectiveAcknowledgementCount() == 0);

  assert(apply(coordinator, 
             receipt(fourteenth, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.acknowledgedThrough() == records[13].identity);
}

void invalidAndBusyPathsDoNotMutate() {
  TestFlash flash;
  TestIncarnationSource incarnation;
  HistoryStore store(flash, &incarnation);
  start(store);

  const auto first = allocate(store, 450000001);
  const auto second = allocate(store, 450000002);
  append(store, first);
  append(store, second);

  HistoryDeliveryCoordinator coordinator(store);

  tlp::BackendDurableReceiptPlaintext empty{};
  assert(apply(coordinator, empty) ==
         HistoryDeliveryCoordinator::ApplyResult::kInvalidReceipt);

  const uint64_t unordered[] = {second.identity, first.identity};
  assert(apply(coordinator, 
             receipt(unordered, 2)) ==
         HistoryDeliveryCoordinator::ApplyResult::kInvalidReceipt);

  // Keep HistoryStore busy with a real append operation.
  const auto pending = allocate(store, 450000003);
  assert(store.append(pending.packet, pending.identity));

  const uint64_t first_only[] = {first.identity};
  assert(apply(coordinator, 
             receipt(first_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kUnavailable);
  assert(coordinator.acknowledgedThrough() == 0);
  assert(coordinator.selectiveAcknowledgementCount() == 0);

  settle(store);
  bool success = false;
  assert(store.takeAppendResult(success) && success);
}


void fullSelectiveSetDrainsAfterCapacityOverwrite() {
  TestFlash flash;
  TestIncarnationSource incarnation;
  HistoryStore store(flash, &incarnation);
  start(store);

  uint64_t selected[HistoryDeliveryCoordinator::kMaxSelectiveAcknowledgements]{};
  for (uint32_t i = 0; i < HistoryStore::capacity(); ++i) {
    const auto record =
        allocate(store, 460000000 + static_cast<int32_t>(i));
    if (i >= kRecordsPerPage &&
        i < kRecordsPerPage +
                HistoryDeliveryCoordinator::kMaxSelectiveAcknowledgements) {
      selected[i - kRecordsPerPage] = record.identity;
    }
    append(store, record);
  }

  HistoryDeliveryCoordinator coordinator(store);
  assert(apply(coordinator,
               receipt(selected, tlp::kHistoryReceiptMaxIdentities)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(apply(coordinator,
               receipt(selected + tlp::kHistoryReceiptMaxIdentities,
                       tlp::kHistoryReceiptMaxIdentities)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.selectiveAcknowledgementCount() ==
         HistoryDeliveryCoordinator::kMaxSelectiveAcknowledgements);
  assert(coordinator.acknowledgedThrough() == 0);

  // One more record rotates/erases page zero. The old missing gap is now gone;
  // selected[0] becomes the oldest surviving actual History record.
  append(store, allocate(store, 460001000));
  assert(store.diagnostics().capacity_lost_undelivered != 0);

  const uint64_t duplicate_oldest[] = {selected[0]};
  assert(apply(coordinator, receipt(duplicate_oldest, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.acknowledgedThrough() ==
         selected[HistoryDeliveryCoordinator::kMaxSelectiveAcknowledgements -
                  1]);
  assert(coordinator.selectiveAcknowledgementCount() == 0);
}

void deliveryApplyNeverWritesFlashOrDurableCheckpoint() {
  TestFlash flash;
  TestIncarnationSource incarnation;
  HistoryStore store(flash, &incarnation);
  start(store);

  const auto first = allocate(store, 470000001);
  const auto second = allocate(store, 470000002);
  append(store, first);
  append(store, second);

  const auto flash_before = flash.bytes;
  const uint64_t durable_before = store.deliveredThrough();

  HistoryDeliveryCoordinator coordinator(store);
  const uint64_t second_only[] = {second.identity};
  const uint64_t first_only[] = {first.identity};
  assert(apply(coordinator, receipt(second_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(apply(coordinator, receipt(first_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);

  assert(store.acknowledgedThrough() == second.identity);
  assert(store.deliveredThrough() == durable_before);
  assert(flash.bytes == flash_before);
}

void transientCommitReadFaultLeavesOnlySafePrefix() {
  bool saw_partial_safe_prefix = false;

  for (uint32_t fail_after = 0; fail_after < 128; ++fail_after) {
    TestFlash flash;
    TestIncarnationSource incarnation;
    HistoryStore store(flash, &incarnation);
    start(store);

    HistoryStore::Record records[4]{};
    for (size_t i = 0; i < 4; ++i) {
      records[i] = allocate(store, 480000000 + static_cast<int32_t>(i));
      append(store, records[i]);
    }

    HistoryDeliveryCoordinator coordinator(store);
    const uint64_t newer[] = {
        records[1].identity, records[2].identity, records[3].identity};
    assert(apply(coordinator, receipt(newer, 3)) ==
           HistoryDeliveryCoordinator::ApplyResult::kApplied);
    assert(coordinator.acknowledgedThrough() == 0);

    const auto flash_before = flash.bytes;
    flash.failOneReadAfter(fail_after);
    const uint64_t oldest[] = {records[0].identity};
    const auto result = apply(coordinator, receipt(oldest, 1));

    // M4P5A is RAM-only even when a transient read fault interrupts commit.
    assert(store.deliveredThrough() == 0);
    assert(flash.bytes == flash_before);

    if (result != HistoryDeliveryCoordinator::ApplyResult::kInvariantFailure)
      continue;

    const uint64_t acknowledged = coordinator.acknowledgedThrough();
    assert(acknowledged <= records[3].identity);
    if (acknowledged != 0) {
      bool exact_safe_prefix = false;
      for (const auto& record : records) {
        if (record.identity == acknowledged) {
          exact_safe_prefix = true;
          break;
        }
      }
      assert(exact_safe_prefix);
    }
    if (acknowledged > 0 && acknowledged < records[3].identity)
      saw_partial_safe_prefix = true;
  }

  // Locks the documented M2 contract: kInvariantFailure may expose a shorter
  // strictly validated RAM prefix, but never unsafe/durable progress.
  assert(saw_partial_safe_prefix);
}

void incarnationRebaselineCannotReuseSelectiveFacts() {
  TestFlash flash;
  TestIncarnationSource incarnation;
  HistoryStore store(flash, &incarnation);
  start(store);

  const auto old_first = allocate(store, 490000001);
  const auto old_second = allocate(store, 490000002);
  append(store, old_first);
  append(store, old_second);

  HistoryDeliveryCoordinator coordinator(store);
  const uint64_t old_second_only[] = {old_second.identity};
  assert(apply(coordinator, receipt(old_second_only, 1), kIncarnation) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.selectiveAcknowledgementCount() == 1);

  // Explicit destructive development re-baseline. New records intentionally
  // reuse local identities 1,2 under a different History incarnation.
  flash.bytes.fill(0xFF);
  incarnation.next = kIncarnation + 1;
  start(store);

  const auto new_first = allocate(store, 490100001);
  const auto new_second = allocate(store, 490100002);
  append(store, new_first);
  append(store, new_second);
  assert(new_first.identity == old_first.identity);
  assert(new_second.identity == old_second.identity);

  const uint64_t new_first_only[] = {new_first.identity};
  assert(apply(coordinator, receipt(new_first_only, 1), kIncarnation) ==
         HistoryDeliveryCoordinator::ApplyResult::kInvalidReceipt);
  assert(coordinator.acknowledgedThrough() == 0);

  assert(apply(coordinator, receipt(new_first_only, 1), kIncarnation + 1) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  // The old-incarnation selective fact for identity 2 must not leak into the
  // new stream and acknowledge new_second without a new receipt.
  assert(coordinator.acknowledgedThrough() == new_first.identity);
  assert(coordinator.selectiveAcknowledgementCount() == 0);
}

void receiptValidationEdges() {
  TestFlash flash;
  TestIncarnationSource incarnation;
  HistoryStore store(flash, &incarnation);
  start(store);
  const auto first = allocate(store, 500000001);
  append(store, first);

  HistoryDeliveryCoordinator coordinator(store);

  tlp::BackendDurableReceiptPlaintext too_many{};
  too_many.count = tlp::kHistoryReceiptMaxIdentities + 1;
  assert(apply(coordinator, too_many) ==
         HistoryDeliveryCoordinator::ApplyResult::kInvalidReceipt);

  const uint64_t zero[] = {0};
  assert(apply(coordinator, receipt(zero, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kInvalidReceipt);

  const uint64_t equal[] = {first.identity, first.identity};
  assert(apply(coordinator, receipt(equal, 2)) ==
         HistoryDeliveryCoordinator::ApplyResult::kInvalidReceipt);

  assert(apply(coordinator, receipt(&first.identity, 1), kIncarnation + 1) ==
         HistoryDeliveryCoordinator::ApplyResult::kInvalidReceipt);

  TestFlash unready_flash;
  TestIncarnationSource unready_incarnation;
  HistoryStore unready_store(unready_flash, &unready_incarnation);
  HistoryDeliveryCoordinator unready_coordinator(unready_store);
  assert(apply(unready_coordinator, receipt(&first.identity, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kUnavailable);
}

}  // namespace

int main() {
  selectiveReceiptWaitsForGapThenDrains();
  numericTicketGapNeedsExplicitLaterIdentity();
  unknownIdentityRejectsAtomically();
  selectiveSetIsBoundedAndFailureIsAtomic();
  invalidAndBusyPathsDoNotMutate();
  fullSelectiveSetDrainsAfterCapacityOverwrite();
  deliveryApplyNeverWritesFlashOrDurableCheckpoint();
  transientCommitReadFaultLeavesOnlySafePrefix();
  incarnationRebaselineCannotReuseSelectiveFacts();
  receiptValidationEdges();
  return 0;
}
