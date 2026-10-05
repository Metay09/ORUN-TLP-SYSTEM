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
    incarnation = kIncarnation;
    return true;
  }
};

class TestFlash final : public FlashBackend {
 public:
  TestFlash() { bytes.fill(0xFF); }

  bool begin() override { return true; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || offset > bytes.size() ||
        size > bytes.size() - offset)
      return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
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
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
             receipt(second_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.acknowledgedThrough() == 0);
  assert(coordinator.selectiveAcknowledgementCount() == 1);

  const uint64_t first_only[] = {first.identity};
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
             receipt(first_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.acknowledgedThrough() == second.identity);
  assert(coordinator.selectiveAcknowledgementCount() == 0);

  const uint64_t first_second[] = {first.identity, second.identity};
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
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
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
             receipt(fourth_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.acknowledgedThrough() == 0);

  const uint64_t first_only[] = {first.identity};
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
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
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
             receipt(second_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.selectiveAcknowledgementCount() == 1);

  const uint64_t bad[] = {first.identity, UINT64_C(999999)};
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
             receipt(bad, 2)) ==
         HistoryDeliveryCoordinator::ApplyResult::kUnknownIdentity);

  // The valid first identity from the same rejected batch must not partially
  // advance delivery or consume the previously selected second identity.
  assert(coordinator.acknowledgedThrough() == 0);
  assert(coordinator.selectiveAcknowledgementCount() == 1);

  const uint64_t first_only[] = {first.identity};
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
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

  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
             receipt(batch_one, tlp::kHistoryReceiptMaxIdentities)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
             receipt(batch_two, tlp::kHistoryReceiptMaxIdentities)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.selectiveAcknowledgementCount() ==
         HistoryDeliveryCoordinator::kMaxSelectiveAcknowledgements);
  assert(coordinator.acknowledgedThrough() == 0);

  const uint64_t fourteenth[] = {records[13].identity};
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
             receipt(fourteenth, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kSelectiveSetFull);
  assert(coordinator.selectiveAcknowledgementCount() ==
         HistoryDeliveryCoordinator::kMaxSelectiveAcknowledgements);
  assert(coordinator.acknowledgedThrough() == 0);

  const uint64_t first_only[] = {records[0].identity};
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
             receipt(first_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kApplied);
  assert(coordinator.acknowledgedThrough() == records[12].identity);
  assert(coordinator.selectiveAcknowledgementCount() == 0);

  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
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
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(empty) ==
         HistoryDeliveryCoordinator::ApplyResult::kInvalidReceipt);

  const uint64_t unordered[] = {second.identity, first.identity};
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
             receipt(unordered, 2)) ==
         HistoryDeliveryCoordinator::ApplyResult::kInvalidReceipt);

  // Keep HistoryStore busy with a real append operation.
  const auto pending = allocate(store, 450000003);
  assert(store.append(pending.packet, pending.identity));

  const uint64_t first_only[] = {first.identity};
  assert(coordinator.applyReplayAcceptedBackendDurableReceipt(
             receipt(first_only, 1)) ==
         HistoryDeliveryCoordinator::ApplyResult::kUnavailable);
  assert(coordinator.acknowledgedThrough() == 0);
  assert(coordinator.selectiveAcknowledgementCount() == 0);

  settle(store);
  bool success = false;
  assert(store.takeAppendResult(success) && success);
}

}  // namespace

int main() {
  selectiveReceiptWaitsForGapThenDrains();
  numericTicketGapNeedsExplicitLaterIdentity();
  unknownIdentityRejectsAtomically();
  selectiveSetIsBoundedAndFailureIsAtomic();
  invalidAndBusyPathsDoNotMutate();
  return 0;
}
