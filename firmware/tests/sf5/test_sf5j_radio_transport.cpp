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
}  // namespace

int main() {
  receiveProductAndRetainExactBytes();
  eventSizeAndTransmitAdmission();
  rejectMalformedTransmit();
  puts("SF5J1 PRODUCT_SECURE radio-owner transport: PASS");
}
