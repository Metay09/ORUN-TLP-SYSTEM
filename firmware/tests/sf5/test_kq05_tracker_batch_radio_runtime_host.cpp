// KQ05 opt-in 0x0A raw RX -> actual SecurityStore V3 HMAC ->
// five separately committed OBS1 markers. Host fake radio mailbox ONLY.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kq05_tracker_batch_radio_runtime.h"

#define main kq05_tracker_selective_fixture_main
#include "test_kq05_tracker_selective_batch_release_host.cpp"
#undef main

namespace {
orun_tlp::Kq05GatewayBatchAckRxFrame g_batch_radio_mailbox{};
bool g_batch_radio_pending=false;
unsigned g_mailbox_discards=0U;

void injectRawAck(const uint8_t* wire,size_t length) {
  assert(!g_batch_radio_pending && wire!=nullptr);
  assert(length<=sizeof(g_batch_radio_mailbox.payload));
  g_batch_radio_mailbox={};
  g_batch_radio_mailbox.size=static_cast<uint16_t>(length);
  g_batch_radio_mailbox.rssi_dbm=-63;
  g_batch_radio_mailbox.snr_db=8;
  memcpy(g_batch_radio_mailbox.payload,wire,length);
  g_batch_radio_pending=true;
}
void resetMailbox() {
  g_batch_radio_mailbox={};
  g_batch_radio_pending=false;
  g_mailbox_discards=0U;
}
} // namespace

namespace orun_tlp {
// Only mailbox methods are host fakes; ReleaseOwner, SecurityStore,
// ProductRecordStore, crypto and actual selective Flash release are REAL.
bool RadioManager::peekKq05GatewayBatchAckFrame(
    Kq05GatewayBatchAckRxFrame* output) const {
  if (!output || !g_batch_radio_pending) return false;
  *output=g_batch_radio_mailbox;
  return true;
}
void RadioManager::discardKq05GatewayBatchAckFrame() {
  g_batch_radio_mailbox={};
  g_batch_radio_pending=false;
  ++g_mailbox_discards;
}
} // namespace orun_tlp

namespace {
using Runtime=orun_tlp::Kq05TrackerBatchRadioRuntime;

void testGenuineFiveRecordRadioIntakeAndDurableRelease() {
  resetMailbox();
  Fixture f;
  f.admitGrant();
  f.cacheUpTo(5U);

  uint8_t valid[ack::kMaxSize]{};
  size_t length=0U;
  buildAck(f.objects,5U,valid,length);
  assert(length==88U);
  orun_tlp::RadioManager radio;
  ReleaseOwner owner(f.security,f.records);
  Runtime intake(radio,owner);
  assert(intake.receive(true,0U)==Runtime::Intake::kNone);
  injectRawAck(valid,length);

  const uint32_t before=f.records.diagnostics().custody_release_committed;
  // When a newer LIVE location has pending radio/storage priority,
  // the raw ACK remains deferred rather than gating any sender.
  assert(intake.receive(false,0U)==Runtime::Intake::kDeferred);
  assert(g_batch_radio_pending && g_mailbox_discards==0U);
  assert(!owner.pending());
  assert(f.records.diagnostics().custody_release_committed==before);
  assert(intake.receive(true,1U)==Runtime::Intake::kStarted);
  assert(!g_batch_radio_pending && g_mailbox_discards==1U);
  assert(owner.pending());
  assert(f.records.diagnostics().custody_release_committed==before);

  // One authenticated group, not an atomic range deletion: five *separate*
  // actual durable OBS1 marker outcomes driven by the same store poll().
  for (uint8_t i=0U;i<5U;++i) {
    assert(owner.service()==ReleaseOwner::Service::kWaitingFlash);
    assert(nextDurableOutcome(owner,f.records)==
           ReleaseOwner::Service::kReleasedOne);
    assert(owner.releasedCount()==i+1U);
  }
  assert(owner.service()==ReleaseOwner::Service::kApplied);
  assert(!owner.pending());
  assert(f.records.diagnostics().custody_release_committed==before+5U);
  for (uint8_t i=0U;i<5U;++i) {
    ObservationStore::Record record{};
    assert(f.records.store().lookup(f.objects[i].identity,record)==
           ObservationStore::LookupResult::kFound);
    assert(record.released && !record.release_uncertain);
  }
  assert(intake.receive(true,2U)==Runtime::Intake::kNone);
}

void testBadMacNeverReleasesOrStaysInMailbox() {
  resetMailbox();
  Fixture f;
  f.admitGrant();
  f.cacheUpTo(5U);

  uint8_t wire[ack::kMaxSize]{};
  size_t length=0U;
  buildAck(f.objects,3U,wire,length);
  wire[length-1U]^=0x80U; // Tamper authenticated tag; shape stays canonical.

  orun_tlp::RadioManager radio;
  ReleaseOwner owner(f.security,f.records);
  Runtime intake(radio,owner);
  injectRawAck(wire,length);
  assert(intake.receive(true,0U)==Runtime::Intake::kRejected);
  assert(!owner.pending() && !g_batch_radio_pending);
  assert(g_mailbox_discards==1U);
  assert(f.records.diagnostics().custody_release_committed==0U);
  bool untouched[5]{};
  assertFiveLogicalFlags(f,untouched);
}

void testNoGrantAndNewAckWaitsForPreviousRelease() {
  resetMailbox();
  Fixture f;
  f.cacheUpTo(5U);
  uint8_t wire[ack::kMaxSize]{};
  size_t length=0U;
  buildAck(f.objects,2U,wire,length);

  orun_tlp::RadioManager radio;
  ReleaseOwner owner(f.security,f.records);
  Runtime intake(radio,owner);
  injectRawAck(wire,length);
  assert(intake.receive(true,0U)==Runtime::Intake::kRejected);
  assert(!owner.pending() && !g_batch_radio_pending);
  assert(f.records.diagnostics().custody_release_committed==0U);

  f.admitGrant();
  injectRawAck(wire,length);
  assert(intake.receive(true,500U)==Runtime::Intake::kStarted);
  assert(owner.pending() && !g_batch_radio_pending);
  // A second mailbox item cannot be submitted while a prior marker owns
  // the shared Flash outcome. The mailbox must survive that condition.
  injectRawAck(wire,length);
  assert(intake.receive(true,501U)==Runtime::Intake::kDeferred);
  assert(g_batch_radio_pending && intake.temporaryAttempts()==0U);
  for (uint8_t i=0U;i<2U;++i) {
    assert(owner.service()==ReleaseOwner::Service::kWaitingFlash);
    assert(nextDurableOutcome(owner,f.records)==
           ReleaseOwner::Service::kReleasedOne);
  }
  assert(owner.service()==ReleaseOwner::Service::kApplied);
  assert(!owner.pending());
  // Replay is not allowed to re-release an already released exact group.
  assert(intake.receive(true,502U)==Runtime::Intake::kRejected);
  assert(!g_batch_radio_pending);
  assert(f.records.diagnostics().custody_release_committed==2U);
}


} // namespace

int main() {
  kq05_tracker_selective_fixture_main();
  testGenuineFiveRecordRadioIntakeAndDurableRelease();
  testBadMacNeverReleasesOrStaysInMailbox();
  testNoGrantAndNewAckWaitsForPreviousRelease();
  puts("KQ05 0x0A raw radio mailbox -> real V3 full-HMAC -> 1..5 OBS1 durable releases: PASS (HOST ONLY)");
  puts("NO PHYSICAL RF, NO BOARD FLASH, NO LEGACY ERASE, NO DEVICE UPLOAD");
  return 0;
}
