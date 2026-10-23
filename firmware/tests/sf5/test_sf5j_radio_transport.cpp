// SF5J1: exercise the real radio owner with the existing R2 driver model.
// Codec-structural validation alone is NOT an AEAD authentication result.
#include <assert.h>
#include <string.h>
#include <stdio.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#endif
#define main sf5j1_prior_r2_main
#include "../r2/test_r2.cpp"
#undef main
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {
void makeProduct(uint8_t family, uint64_t source, uint8_t* bytes, size_t size) {
  orun_tlp::tlp::ProductSecureFrame frame{};
  frame.app_family = family;
  frame.path_flags = orun_tlp::tlp::kProductPathFlagCustodyRequested;
  frame.ciphertext_len = static_cast<uint8_t>(
      family == orun_tlp::tlp::kProductAppFamilyPeriodic
          ? orun_tlp::tlp::kPeriodicPlaintextSize
          : orun_tlp::tlp::kEventPlaintextSize);
  frame.device_id = source;
  frame.key_epoch = 7;
  frame.security_counter = 42;
  assert(orun_tlp::tlp::encodeProductSecureFrame(frame, bytes, size) ==
         orun_tlp::tlp::ProductSecureCodecResult::kOk);
}

void receiveProductAndRetainExactBytes() {
  RadioManager manager;
  TestSequence sequence;
  beginAs(manager, sequence, NodeRole::kBase);

  uint8_t wire[orun_tlp::tlp::kPeriodicFrameSize]{};
  makeProduct(orun_tlp::tlp::kProductAppFamilyPeriodic, 0x2222, wire,
              sizeof(wire));
  uint8_t expected[sizeof(wire)]{};
  memcpy(expected, wire, sizeof(wire));

  rxDone(wire, sizeof(wire), -67, 8);
  memset(wire, 0, sizeof(wire));  // Driver callback memory is ephemeral.
  manager.update(false);

  auto diag = manager.eventDiagnostics();
  assert(diag.product_secure_rx_queued == 1);
  assert(diag.product_secure_rx_drops == 0);
  orun_tlp::ProductSecureRxFrame out{};
  assert(manager.takeProductSecureFrame(&out));
  assert(out.size == sizeof(expected));
  assert(out.rssi_dbm == -67 && out.snr_db == 8);
  assert(memcmp(out.payload, expected, sizeof(expected)) == 0);
  assert(!manager.takeProductSecureFrame(&out));

  // Mailbox never silently overwrites the first pending exact frame.
  rxDone(expected, sizeof(expected), -67, 8);
  manager.update(false);
  rxDone(expected, sizeof(expected), -70, 4);
  manager.update(false);
  diag = manager.eventDiagnostics();
  assert(diag.product_secure_rx_queued == 2);
  assert(diag.product_secure_rx_drops == 1);
  assert(manager.takeProductSecureFrame(&out));
  assert(memcmp(out.payload, expected, sizeof(expected)) == 0);

  // Invalid reserved byte must not fall through to the legacy decoder.
  expected[7] = 1;
  rxDone(expected, sizeof(expected), -70, 4);
  manager.update(false);
  diag = manager.eventDiagnostics();
  assert(diag.product_secure_rx_drops == 2);
  assert(!manager.takeProductSecureFrame(&out));
  assert(manager.baseDiagnostics().direct_observations == 0);
}

void eventSizeAndTransmitAdmission() {
  RadioManager manager;
  TestSequence sequence;
  beginAs(manager, sequence, NodeRole::kTracker);
  uint8_t wire[orun_tlp::tlp::kEventFrameSize]{};
  makeProduct(orun_tlp::tlp::kProductAppFamilyEvent, kLocalDevice, wire,
              sizeof(wire));

  assert(manager.sendProductSecurePacket(wire, sizeof(wire)));
  assert(manager.isTransmitting());
  assert(last_tx_size == sizeof(wire));
  assert(memcmp(last_tx, wire, sizeof(wire)) == 0);
  assert(manager.eventDiagnostics().product_secure_tx_attempts == 1);
  terminal(false);
  manager.update(false);
  assert(!manager.isTransmitting());

  makeProduct(orun_tlp::tlp::kProductAppFamilyEvent, 0x9999, wire,
              sizeof(wire));
  assert(!manager.sendProductSecurePacket(wire, sizeof(wire)));
  assert(manager.eventDiagnostics().product_secure_tx_attempts == 1);
}

void rejectMalformedTransmit() {
  RadioManager manager;
  TestSequence sequence;
  beginAs(manager, sequence, NodeRole::kTracker);
  uint8_t wire[orun_tlp::tlp::kPeriodicFrameSize]{};
  makeProduct(orun_tlp::tlp::kProductAppFamilyPeriodic, kLocalDevice, wire,
              sizeof(wire));
  wire[5] = 0xFF;  // Unknown security profile.
  assert(!manager.sendProductSecurePacket(wire, sizeof(wire)));
  assert(send_calls == 0);
}
#ifdef ORUN_SF5K3K_ACK_RX_TEST
void ackBe32(uint8_t* b, uint32_t v) {
  for (unsigned i=0U;i<4U;++i)
    b[i]=static_cast<uint8_t>(v>>(24U-8U*i));
}
void ackBe64(uint8_t* b, uint64_t v) {
  for (unsigned i=0U;i<8U;++i)
    b[i]=static_cast<uint8_t>(v>>(56U-8U*i));
}

void makeAck(uint8_t (&wire)[orun_tlp::custody_ack::kWireSize],
             uint64_t tracker) {
  memset(wire,0U,sizeof(wire));
  wire[0]=0x02U;
  wire[1]=0x05U;
  wire[2]=0x01U;
  wire[3]=100U;
  ackBe64(wire+4U,tracker);
  ackBe64(wire+12U,0x1112131415161718ULL);
  ackBe32(wire+20U,7U);
  ackBe32(wire+24U,0U);
  ackBe32(wire+28U,1U);
  for(unsigned i=0U;i<16U;++i)wire[40U+i]=static_cast<uint8_t>(i+3U);
}

void custodyAckTrackerMailboxAndRoleIsolation() {
  RadioManager manager;
  TestSequence seq;
  beginAs(manager,seq,NodeRole::kTracker);
  uint8_t ack[orun_tlp::custody_ack::kWireSize]{};
  makeAck(ack,kLocalDevice);
  uint8_t saved[sizeof(ack)]{};
  memcpy(saved,ack,sizeof(ack));
  rxDone(ack,sizeof(ack),-72,5);
  memset(ack,0U,sizeof(ack)); // callback buffer was transient
  manager.update(false);
  orun_tlp::CustodyAckRxFrame result{};
  assert(manager.takeCustodyAckFrame(&result));
  assert(result.size==sizeof(saved));
  assert(result.rssi_dbm==-72 && result.snr_db==5);
  assert(memcmp(result.payload,saved,sizeof(saved))==0);
  assert(!manager.takeCustodyAckFrame(&result));

  rxDone(saved,sizeof(saved),-50,7);
  manager.update(false);
  rxDone(saved,sizeof(saved),-80,0);
  manager.update(false);
  assert(manager.eventDiagnostics().custody_ack_rx_queued==2U);
  assert(manager.eventDiagnostics().custody_ack_rx_drops==1U);
  assert(manager.takeCustodyAckFrame(&result));
  assert(memcmp(result.payload,saved,sizeof(saved))==0);

  uint8_t invalid[sizeof(saved)]{};
  memcpy(invalid,saved,sizeof(saved));
  invalid[2]=0x80U; // reserved flags
  rxDone(invalid,sizeof(invalid),-80,0);
  manager.update(false);
  memcpy(invalid,saved,sizeof(saved));
  invalid[11]^=1U; // wrong Tracker
  rxDone(invalid,sizeof(invalid),-80,0);
  manager.update(false);
  assert(manager.eventDiagnostics().custody_ack_rx_drops==3U);
  assert(!manager.takeCustodyAckFrame(&result));

  // No pending ACK can survive a role transition.
  rxDone(saved,sizeof(saved),-72,5);
  manager.update(false);
  manager.setRole(NodeRole::kBase);
  manager.update(false);
  assert(!manager.takeCustodyAckFrame(&result));
  rxDone(saved,sizeof(saved),-70,3);
  manager.update(false);
  assert(!manager.takeCustodyAckFrame(&result));
  assert(manager.eventDiagnostics().custody_ack_rx_drops>=4U);
}
#endif

}  // namespace

int main() {
  receiveProductAndRetainExactBytes();
  eventSizeAndTransmitAdmission();
  rejectMalformedTransmit();
#ifdef ORUN_SF5K3K_ACK_RX_TEST
  custodyAckTrackerMailboxAndRoleIsolation();
  puts("SF5K3K Tracker untrusted custody ACK RX mailbox: PASS");
#else
  puts("SF5J1 PRODUCT_SECURE radio-owner transport: PASS");
#endif
}
