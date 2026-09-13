#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "gnss_manager.h"
#include "history_store.h"
#include "position_flow.h"

using namespace orun_tlp;
using namespace orun_tlp::journal_format;
using namespace orun_tlp::storage_config;

constexpr uint64_t kDevice = 0x123456789ABCDEF0ULL;

// Models synchronous NOR operations, not SoftDevice asynchronous completion.
class FaultFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  int64_t program_budget = -1;
  int64_t program_bit_budget = -1;
  int64_t erase_budget = -1;
  uint32_t program_operations = 0;
  uint32_t erase_operations = 0;
  mutable bool fail_next_read = false;

  FaultFlash() { bytes.fill(0xFF); }
  bool begin() override { return true; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (fail_next_read) {
      fail_next_read = false;
      return false;
    }
    if (data == nullptr || offset > bytes.size() ||
        size > bytes.size() - offset) return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  bool program(uint32_t offset, const void* data, size_t size) override {
    ++program_operations;
    if (data == nullptr || size == 0 || (offset & 3U) != 0 ||
        (size & 3U) != 0 || offset > bytes.size() ||
        size > bytes.size() - offset) return false;
    const auto* source = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index) {
      if ((bytes[offset + index] & source[index]) != source[index]) return false;
      if (program_budget == 0) return false;
      for (int bit = 7; bit >= 0; --bit) {
        const uint8_t mask = uint8_t(1U << bit);
        if ((bytes[offset + index] & mask) && !(source[index] & mask)) {
          if (program_bit_budget == 0) return false;
          bytes[offset + index] &= uint8_t(~mask);
          if (program_bit_budget > 0) --program_bit_budget;
        }
      }
      if (program_budget > 0) --program_budget;
    }
    return true;
  }

  bool erasePage(uint32_t page) override {
    ++erase_operations;
    if (page >= kPageCount) return false;
    const size_t first = size_t(page) * kPageSize;
    for (size_t index = 0; index < kPageSize; ++index) {
      if (erase_budget == 0) return false;
      bytes[first + index] = 0xFF;
      if (erase_budget > 0) --erase_budget;
    }
    return true;
  }
};

void settle(HistoryStore& store) {
  for (unsigned pass = 0; pass < 80 && store.busy(); ++pass) store.poll();
  assert(!store.busy());
}

void start(HistoryStore& store) {
  assert(store.begin(kDevice));
  settle(store);
  assert(store.ready() && store.canAppend());
}

HistoryStore::Record allocate(HistoryStore& store,
                              int32_t latitude = 410000000) {
  uint32_t sequence = 0;
  HistoryStore::Record record;
  assert(store.nextSequence(sequence, record.identity));
  const tlp::PositionPacket packet{kDevice, sequence, 0, latitude, -290000000,
                                   -10, 123, 8, 5};
  assert(tlp::serializePositionPacket(packet, record.packet,
                                      sizeof(record.packet)));
  return record;
}

void append(HistoryStore& store, const HistoryStore::Record& record) {
  assert(store.append(record.packet, record.identity));
  settle(store);
  bool success = false;
  assert(store.takeAppendResult(success) && success);
}

void ordered(const HistoryStore& store) {
  HistoryStore::Record record;
  uint64_t cursor = 0;
  uint32_t count = 0;
  while (store.readAfter(cursor, record)) {
    assert(record.identity > cursor);
    cursor = record.identity;
    ++count;
  }
  assert(count == store.count());
}

void virginNormalAppendAndWear() {
  FaultFlash flash;
  HistoryStore store(flash);
  start(store);
  assert(store.count() == 0);
  const uint32_t erases = flash.erase_operations;
  const uint32_t programs = flash.program_operations;
  append(store, allocate(store));
  append(store, allocate(store));
  assert(flash.erase_operations == erases);
  assert(flash.program_operations - programs == 4);
}

void firstInitializationPowerLoss() {
  for (int cut = 0; cut <= int(kStaticHeaderSize); ++cut) {
    FaultFlash flash;
    HistoryStore interrupted(flash);
    assert(interrupted.begin(kDevice));
    flash.program_budget = cut;
    settle(interrupted);
    flash.program_budget = -1;
    HistoryStore rebooted(flash);
    start(rebooted);
    assert(rebooted.count() == 0);
    append(rebooted, allocate(rebooted));
  }
}

void recordPowerLossPreservesCommittedDataAndSequence() {
  FaultFlash baseline;
  HistoryStore original(baseline);
  start(original);
  const auto committed = allocate(original);
  append(original, committed);

  for (int cut = 0; cut <= int(kRecordSize); ++cut) {
    FaultFlash flash = baseline;
    HistoryStore interrupted(flash);
    start(interrupted);
    const auto pending = allocate(interrupted);
    flash.program_budget = cut;
    assert(interrupted.append(pending.packet, pending.identity));
    settle(interrupted);
    flash.program_budget = -1;
    HistoryStore rebooted(flash);
    start(rebooted);
    HistoryStore::Record recovered;
    assert(rebooted.lookup(committed.identity, recovered));
    assert(rebooted.lookup(pending.identity, recovered) ==
           (cut == int(kRecordSize)));
    assert(allocate(rebooted).identity > pending.identity);
  }
}

void reservationPowerLossNeverReusesTickets() {
  FaultFlash baseline;
  HistoryStore initial(baseline);
  start(initial);
  const auto used = allocate(initial);
  for (int cut = 0; cut <= int(kSequenceSlotSize); ++cut) {
    FaultFlash flash = baseline;
    HistoryStore rebooting(flash);
    assert(rebooting.begin(kDevice));
    flash.program_budget = cut;
    settle(rebooting);
    flash.program_budget = -1;
    HistoryStore final_store(flash);
    start(final_store);
    assert(allocate(final_store).identity > used.identity);
  }
}

void compactSemanticCorruptionAndIdentityBoundaries() {
  FaultFlash flash;
  HistoryStore store(flash);
  start(store);
  const auto first = allocate(store, 412345678);
  append(store, first);
  HistoryStore::Record recovered;
  assert(store.newest(recovered));
  assert(memcmp(first.packet, recovered.packet, sizeof(first.packet)) == 0);
  assert(kRecordSize == 36 && kPageHeaderSize == 352 &&
         kRecordsPerPage == 104 && kCapacity == 728);

  flash.bytes[kPageHeaderSize + 28] ^= 1;
  HistoryStore corrupted(flash);
  start(corrupted);
  assert(corrupted.count() == 0);
  assert(corrupted.diagnostics().recovery_corruptions == 1);

  const uint64_t identities[] = {0xFFFFFFFEULL, 0xFFFFFFFFULL,
                                 0x100000000ULL, 0x100000001ULL};
  for (uint64_t identity : identities) {
    const tlp::PositionPacket packet{kDevice, uint32_t(identity - 1), 0,
                                     410000000, -290000000, -10, 123, 8, 5};
    HistoryStore::Record input{identity, {}};
    assert(tlp::serializePositionPacket(packet, input.packet,
                                        sizeof(input.packet)));
    uint8_t bytes[kRecordSize];
    encodeRecord(input, bytes);
    HistoryStore::Record output;
    assert(decodeRecord(bytes, kDevice, output));
    assert(output.identity == identity);
    assert(memcmp(input.packet, output.packet, sizeof(input.packet)) == 0);
  }
}

void reservationExhaustionRenewsAutomatically() {
  FaultFlash flash;
  HistoryStore store(flash);
  start(store);
  uint32_t sequence = 0;
  uint64_t identity = 0;
  for (uint64_t expected = 1; expected <= kSequenceBlockSize; ++expected) {
    assert(store.nextSequence(sequence, identity));
    assert(identity == expected);
  }
  assert(!store.nextSequence(sequence, identity));
  store.poll();
  settle(store);
  assert(store.nextSequence(sequence, identity));
  assert(identity == kSequenceBlockSize + 1);
  assert(sequence == uint32_t(identity - 1));
}

void circularWrapAndPageTransitionCuts() {
  FaultFlash flash;
  HistoryStore store(flash);
  start(store);
  for (unsigned index = 0; index < kCapacity + 120; ++index)
    append(store, allocate(store));
  assert(store.count() <= kCapacity);
  assert(store.count() >= kCapacity - kRecordsPerPage);
  ordered(store);

  HistoryStore rebooted(flash);
  start(rebooted);
  while (rebooted.count() < kCapacity)
    append(rebooted, allocate(rebooted));
  const auto pending = allocate(rebooted);
  flash.erase_budget = kPageSize / 2;
  assert(rebooted.append(pending.packet, pending.identity));
  settle(rebooted);
  flash.erase_budget = -1;
  HistoryStore after_cut(flash);
  start(after_cut);
  assert(after_cut.count() >= 6 * kRecordsPerPage);
  ordered(after_cut);
}

void pageRotationUsesOneErase() {
  FaultFlash flash;
  HistoryStore store(flash);
  start(store);
  for (unsigned index = 0; index < kRecordsPerPage; ++index)
    append(store, allocate(store));
  const uint32_t erases = flash.erase_operations;
  const uint32_t programs = flash.program_operations;
  append(store, allocate(store));
  assert(flash.erase_operations - erases == 1);
  assert(flash.program_operations - programs == 6);
}

FaultFlash* tx_flash = nullptr;
bool radio_available = true;
unsigned sends = 0;

bool RadioManager::begin(SequenceSource& source) {
  sequences_ = &source;
  device_id_ = kDevice;
  return true;
}
bool RadioManager::canSend() const { return radio_available; }
bool RadioManager::encodePosition(const GnssFix& fix, uint8_t* output,
                                  uint64_t& identity) {
  uint32_t sequence = 0;
  if (!sequences_->nextSequence(sequence, identity)) return false;
  const tlp::PositionPacket packet{kDevice, sequence, fix.utc_epoch_seconds,
                                   fix.latitude_e7, fix.longitude_e7,
                                   fix.altitude_mm, fix.hdop_x100,
                                   fix.satellites, fix.flags};
  return tlp::serializePositionPacket(packet, output,
                                      tlp::kPositionPacketSize);
}
bool RadioManager::sendPositionPacket(const uint8_t* bytes) {
  HistoryStore disk(*tx_flash);
  start(disk);
  HistoryStore::Record record;
  assert(disk.newest(record));
  assert(memcmp(bytes, record.packet, sizeof(record.packet)) == 0);
  ++sends;
  return true;
}

void storeFirstAndPageTransitionFailureCompletes() {
  FaultFlash flash;
  tx_flash = &flash;
  sends = 0;
  HistoryStore store(flash);
  start(store);
  RadioManager radio;
  assert(radio.begin(store));
  PositionFlow flow(store, radio);
  const GnssFix fix{0, 410000000, 290000000, 10, 100, 8, 5};
  assert(flow.acceptFix(fix, 0));
  settle(store);
  assert(flow.update(1) == PositionFlow::Event::kStored);
  assert(sends == 1 && store.backlogCount() == 1);
  assert(store.deliveredThrough() == 0);

  while (store.count() < kRecordsPerPage) append(store, allocate(store));
  assert(flow.acceptFix(fix, 2));
  flash.erase_budget = 0;
  settle(store);
  flash.erase_budget = -1;
  assert(flow.update(3) == PositionFlow::Event::kStorageFailure);
  assert(!flow.pending());
  assert(sends == 1);
}


enum class TransitionFault {
  kErase,
  kHeaderBody,
  kHeaderCommit,
  kHeaderReadback,
  kReservationBody,
  kReservationCommit,
  kReservationReadback,
  kRecordBody,
  kRecordCommit,
  kRecordReadback,
};

void injectTransitionFault(HistoryStore& store, FaultFlash& flash,
                           TransitionFault fault) {
  if (fault == TransitionFault::kErase) {
    flash.erase_budget = 0;
    store.poll();
    return;
  }
  store.poll();  // Page erase.
  if (fault == TransitionFault::kHeaderBody ||
      fault == TransitionFault::kHeaderCommit ||
      fault == TransitionFault::kHeaderReadback) {
    if (fault == TransitionFault::kHeaderReadback)
      flash.fail_next_read = true;
    else
      flash.program_budget =
          fault == TransitionFault::kHeaderBody ? 0 : kStaticHeaderSize - 4;
    store.poll();
    return;
  }
  store.poll();  // Page header body and commit.
  if (fault == TransitionFault::kReservationBody ||
      fault == TransitionFault::kReservationCommit ||
      fault == TransitionFault::kReservationReadback) {
    if (fault == TransitionFault::kReservationReadback)
      flash.fail_next_read = true;
    else
      flash.program_budget = fault == TransitionFault::kReservationBody
                                 ? 0
                                 : kSequenceSlotSize - 4;
    store.poll();
    return;
  }
  store.poll();  // Reservation body and commit.
  if (fault == TransitionFault::kRecordReadback)
    flash.fail_next_read = true;
  else
    flash.program_budget =
        fault == TransitionFault::kRecordBody ? 0 : kRecordSize - 4;
  store.poll();
}

void everyPageTransitionFailureCompletesPositionFlow() {
  FaultFlash baseline;
  HistoryStore preparing(baseline);
  start(preparing);
  for (unsigned index = 0; index < kRecordsPerPage; ++index)
    append(preparing, allocate(preparing));

  const TransitionFault faults[] = {
      TransitionFault::kErase,          TransitionFault::kHeaderBody,
      TransitionFault::kHeaderCommit,   TransitionFault::kHeaderReadback,
      TransitionFault::kReservationBody,
      TransitionFault::kReservationCommit,
      TransitionFault::kReservationReadback, TransitionFault::kRecordBody,
      TransitionFault::kRecordCommit,   TransitionFault::kRecordReadback,
  };
  for (TransitionFault fault : faults) {
    FaultFlash flash = baseline;
    tx_flash = &flash;
    sends = 0;
    HistoryStore store(flash);
    start(store);
    RadioManager radio;
    assert(radio.begin(store));
    PositionFlow flow(store, radio);
    const GnssFix fix{0, 410000000, 290000000, 10, 100, 8, 5};
    assert(flow.acceptFix(fix, 100));
    injectTransitionFault(store, flash, fault);
    assert(!store.busy());
    assert(flow.update(101) == PositionFlow::Event::kStorageFailure);
    assert(!flow.pending());
    assert(sends == 0);
    flash.program_budget = -1;
    flash.erase_budget = -1;
    HistoryStore rebooted(flash);
    start(rebooted);
    assert(rebooted.count() >= kRecordsPerPage);
    assert(rebooted.count() <= kRecordsPerPage + 1);
    HistoryStore::Record recovered;
    for (uint64_t identity = 1; identity <= kRecordsPerPage; ++identity)
      assert(rebooted.lookup(identity, recovered));
  }
}

void cursorDelivery() {
  FaultFlash flash;
  HistoryStore store(flash);
  start(store);
  const auto first = allocate(store);
  append(store, first);
  const auto second = allocate(store);
  append(store, second);
  HistoryStore::Record record;
  assert(store.saveReplayCursor(first.identity));
  settle(store);
  assert(store.getNextBacklog(record) && record.identity == second.identity);
  assert(store.markDeliveredThrough(first.identity));
  settle(store);
  assert(store.backlogCount() == 1);
  HistoryStore rebooted(flash);
  start(rebooted);
  assert(rebooted.deliveredThrough() == first.identity);
  assert(rebooted.replayCursor() == first.identity);
}

void lastTicketPositionAndReservationCuts() {
  FaultFlash flash;
  HistoryStore store(flash);
  start(store);
  uint32_t sequence;
  uint64_t identity;
  for (unsigned n = 0; n < 255; ++n) {
    assert(store.nextSequence(sequence, identity)); // TEST-style consumption.
    assert(identity == n + 1 && sequence == n);
  }
  RadioManager radio;
  assert(radio.begin(store));
  PositionFlow flow(store, radio);
  radio_available = false;
  sends = 0;
  const GnssFix fix{0, 410000000, 290000000, 10, 100, 8, 5};
  const auto erases = flash.erase_operations;
  assert(flow.acceptFix(fix, 10));
  assert(!store.nextSequence(sequence, identity));
  settle(store);
  assert(flow.update(11, false) == PositionFlow::Event::kStored);
  assert(!flow.pending() && sends == 0);
  HistoryStore::Record last;
  assert(store.lookup(256, last));
  assert(flash.erase_operations == erases);
  FaultFlash committed = flash;
  HistoryStore rebooted(committed);
  start(rebooted);
  assert(rebooted.lookup(256, last));
  assert(rebooted.nextSequence(sequence, identity) && identity == 257);

  // Every body/commit byte cut of the next reservation after ticket 256.
  for (int cut = 0; cut <= int(kSequenceSlotSize); ++cut) {
    FaultFlash interrupted = flash;
    HistoryStore renewing(interrupted);
    assert(renewing.begin(kDevice));
    assert(!renewing.nextSequence(sequence, identity));
    interrupted.program_budget = cut;
    settle(renewing);
    interrupted.program_budget = -1;
    HistoryStore recovered(interrupted);
    start(recovered);
    assert(recovered.lookup(256, last));
    assert(recovered.nextSequence(sequence, identity) && identity > 256);
  }
  assert(!store.nextSequence(sequence, identity));
  store.poll(); // Begin next block, still not durable.
  assert(!store.nextSequence(sequence, identity));
  settle(store);
  assert(flash.erase_operations == erases);
  assert(store.nextSequence(sequence, identity) && identity == 257);
  radio_available = true;
}

void allocatedIdentityWrapRecovery() {
  FaultFlash flash;
  uint8_t header[kStaticHeaderSize], reservation[kSequenceSlotSize];
  encodePage(1, kDevice, header);
  encodeSequenceEnd(0xFFFFFF00ULL, reservation);
  assert(flash.program(0, header, sizeof(header)));
  assert(flash.program(kStaticHeaderSize, reservation, sizeof(reservation)));
  HistoryStore store(flash);
  start(store); // Allocates from 0xFFFFFF00 after reserving to 0x100000000.
  uint32_t sequence;
  uint64_t identity;
  for (unsigned n = 0; n < 253; ++n)
    assert(store.nextSequence(sequence, identity));
  const uint64_t ids[] = {0xFFFFFFFEULL, 0xFFFFFFFFULL,
                          0x100000000ULL, 0x100000001ULL};
  for (uint64_t expected : ids) {
    if (!store.canAppend()) { store.poll(); settle(store); }
    const auto record = allocate(store);
    assert(record.identity == expected);
    append(store, record);
    FaultFlash snapshot = flash;
    HistoryStore rebooted(snapshot);
    start(rebooted);
    HistoryStore::Record recovered;
    assert(rebooted.lookup(expected, recovered));
    assert(memcmp(record.packet, recovered.packet, sizeof(record.packet)) == 0);
    assert(rebooted.nextSequence(sequence, identity) && identity > expected);
  }
  assert(store.count() == 4);
  ordered(store);
}

void bitPartialFirstHeaderAndVersionPolicy() {
  uint8_t header[kStaticHeaderSize];
  encodePage(1, kDevice, header);
  for (int cut = 0; cut <= 64; ++cut) {
    FaultFlash flash;
    flash.program_bit_budget = cut;
    assert(!flash.program(0, header, sizeof(header)));
    if (cut == 1) assert(flash.bytes[0] == 0x7F);
    flash.program_bit_budget = -1;
    HistoryStore recovered(flash);
    start(recovered);
    append(recovered, allocate(recovered));
  }
  // CRC and commit tears, including bit-partial words after a valid magic.
  for (unsigned offset : {56U, 60U}) {
    FaultFlash flash;
    assert(flash.program(0, header, offset));
    flash.program_bit_budget = 1;
    assert(!flash.program(offset, header + offset, 4));
    flash.program_bit_budget = -1;
    HistoryStore recovered(flash);
    start(recovered);
    assert(recovered.count() == 0);
  }
  FaultFlash flash;
  HistoryStore original(flash);
  start(original);
  const auto record = allocate(original);
  append(original, record);
  flash.program_bit_budget = 1;
  assert(!flash.program(kPageSize, header, sizeof(header)));
  flash.program_bit_budget = -1;
  const auto erases = flash.erase_operations;
  HistoryStore recovered(flash);
  start(recovered);
  HistoryStore::Record output;
  assert(recovered.lookup(record.identity, output));
  assert(flash.erase_operations == erases);

  FaultFlash old;
  header[4] = 2;
  put32(header + 56, crc32(header, 56));
  assert(old.program(0, header, sizeof(header)));
  uint64_t generation;
  assert(!decodePage(header, kDevice, generation));
  const auto before = old.bytes;
  HistoryStore unsupported(old);
  assert(!unsupported.begin(kDevice));
  unsupported.poll();
  assert(old.bytes == before && old.erase_operations == 0);
  // Old-format debris must not cause the valid v3 history to be reset.
  assert(flash.program(2 * kPageSize, header, sizeof(header)));
  HistoryStore mixed(flash);
  start(mixed);
  assert(mixed.lookup(record.identity, output));
  assert(flash.erase_operations == erases);
}

int main() {
  lastTicketPositionAndReservationCuts();
  allocatedIdentityWrapRecovery();
  bitPartialFirstHeaderAndVersionPolicy();
  virginNormalAppendAndWear();
  firstInitializationPowerLoss();
  recordPowerLossPreservesCommittedDataAndSequence();
  reservationPowerLossNeverReusesTickets();
  compactSemanticCorruptionAndIdentityBoundaries();
  reservationExhaustionRenewsAutomatically();
  circularWrapAndPageTransitionCuts();
  pageRotationUsesOneErase();
  storeFirstAndPageTransitionFailureCompletes();
  everyPageTransitionFailureCompletesPositionFlow();
  cursorDelivery();
  puts("M4 storage repair regression checks: PASS");
}
