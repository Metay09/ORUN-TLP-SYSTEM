#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <SX126x-Arduino.h>
#include <semphr.h>
#include "driver_patch_model.h"
#include "radio_driver_gate.h"
#include "radio_config.h"
#include "gnss_config.h"

#include "network_service.h"
#include "radio_manager.h"
#include "relay_config.h"
#include "tlp_position_packet.h"
#include "tlp_relay_forward_packet.h"
#include "tlp_test_packet.h"

using namespace orun_tlp;

namespace {

constexpr uint64_t kLocalDevice = 0x0102030405060708ULL;
RadioEvents_t* callbacks = nullptr;
RadioState_t radio_state = RF_IDLE;
uint32_t rx_calls = 0;
uint32_t send_calls = 0;
uint32_t standby_calls = 0;
uint32_t sleep_calls = 0;
uint8_t last_tx[255]{};
uint8_t last_tx_size = 0;
uint32_t test_now = 0;
uint32_t send_cost_ms = 0;
bool pending_rx = false;
bool rx_arrives_before_standby = false;
uint8_t pending_packet[34]{};
unsigned driver_calls = 0;
void driverEntry() { requireDriverGate(); ++driver_calls; }

void initRadio(RadioEvents_t* events) { driverEntry(); callbacks = events; }
RadioState_t getStatus() { driverEntry(); return radio_state; }
void setChannel(uint32_t) { driverEntry(); }
void setRxConfig(RadioModems_t, uint32_t, uint32_t, uint8_t, uint32_t,
                 uint16_t, uint16_t, bool, uint8_t, bool, bool, uint8_t, bool,
                 bool) { driverEntry(); }
void setTxConfig(RadioModems_t, int8_t, uint32_t, uint32_t, uint32_t, uint8_t,
                 uint16_t, bool, bool, bool, uint8_t, bool, uint32_t) { driverEntry(); }
void send(uint8_t* payload, uint8_t size) {
  driverEntry();
  ++send_calls;
  last_tx_size = size;
  memcpy(last_tx, payload, size);
  radio_state = RF_TX_RUNNING;
  test_now += send_cost_ms;
}
void sleepRadio() {
  driverEntry();
  ++sleep_calls;
  radio_state = RF_IDLE;
}
void standbyRadio() {
  driverEntry();
  ++standby_calls;
  if (rx_arrives_before_standby) {
    // A final old-session packet arrives after owner dispatch, before stop.
    pending_rx = true;
    IrqFired = true;
    rx_arrives_before_standby = false;
  }
  radio_state = RF_IDLE;
}
void receive(uint32_t) {
  driverEntry();
  ++rx_calls;
  radio_state = RF_RX_RUNNING;
}
void setSyncWord(uint16_t) { driverEntry(); }
uint16_t getSyncWord() { driverEntry(); return 0x1424; }

// Invoke the real application callbacks inside the same production gate used
// by patched dispatch. Task 2 may stay paused with the gate held while task 1
// makes arbitrarily many owner attempts; no scheduler or time guess is used.
void rxDone(uint8_t* packet, uint16_t size, int16_t rssi, int8_t snr) {
  radio_driver::Guard gate;
  assert(gate);
  callbacks->RxDone(packet, size, rssi, snr);
}
void terminal(bool timeout, uint32_t token = 0) {
  radio_driver::Guard gate;
  assert(gate);
  const auto current = radio_driver::generation();
  if (token) radio_driver::setGeneration(token); // Inject captured old token.
  if (!token || token == current) radio_state = RF_IDLE;
  if (timeout) callbacks->TxTimeout(); else callbacks->TxDone();
  radio_driver::setGeneration(current);
}

class TestSequence : public SequenceSource {
 public:
  bool nextSequence(uint32_t& sequence, uint64_t& identity) override {
    sequence = next_++;
    identity = sequence + 1;
    return true;
  }

 private:
  uint32_t next_ = 0;
};

void resetFakeRadio() {
  Serial.output.clear();
  callbacks = nullptr;
  radio_state = RF_IDLE;
  rx_calls = 0;
  send_calls = 0;
  standby_calls = 0;
  sleep_calls = 0;
  last_tx_size = 0;
  memset(last_tx, 0, sizeof(last_tx));
  test_now = 0;
  send_cost_ms = 0;
  pending_rx = false;
  rx_arrives_before_standby = false;
  IrqFired = false;
  TimerTxTimeout = TimerRxTimeout = false;
  driver_calls = 0;
}

void makePosition(uint64_t source, uint32_t sequence, uint8_t* bytes) {
  const tlp::PositionPacket packet{
      source, sequence, 1700000000, 410000000, 290000000, 12345, 175, 9, 7};
  assert(tlp::serializePositionPacket(packet, bytes, tlp::kPositionPacketSize));
}

void beginAs(RadioManager& manager, TestSequence& sequences, NodeRole role) {
  resetFakeRadio();
  assert(manager.begin(sequences));
  assert(callbacks != nullptr && radio_state == RF_RX_RUNNING && rx_calls == 1);
  manager.setRole(role);
  if (role != NodeRole::kBase) {
    manager.update(false);
    manager.update(false);
  }
}

void sendLocalPosition(RadioManager& manager, uint32_t sequence) {
  uint8_t bytes[tlp::kPositionPacketSize];
  makePosition(kLocalDevice, sequence, bytes);
  assert(manager.sendPositionPacket(bytes));
  assert(manager.isTransmitting() && radio_state == RF_TX_RUNNING);
}

void callbackOwnershipPayloadCopyAndOverflow() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kRelay);

  uint8_t packet[tlp::kPositionPacketSize];
  makePosition(0x1111, 1, packet);
  rxDone(packet, sizeof(packet), -101, 3);
  assert(manager.relayDiagnostics().valid_packets_received == 0);
  memset(packet, 0xA5, sizeof(packet));
  manager.update(false);
  assert(manager.relayDiagnostics().valid_packets_received == 1);
  assert(manager.relayDiagnostics().queued == 1);

  // Start fresh and fill the callback handoff without letting the owner run.
  beginAs(manager, sequences, NodeRole::kRelay);
  for (uint32_t sequence = 10; sequence < 15; ++sequence) {
    makePosition(0x2222, sequence, packet);
    rxDone(packet, sizeof(packet), -95, 6);
  }
  assert(manager.relayDiagnostics().valid_packets_received == 0);
  assert(manager.eventDiagnostics().rx_queue_drops == 1);
  manager.update(false);
  assert(manager.relayDiagnostics().valid_packets_received == 4);
  assert(manager.relayDiagnostics().queued == relay_config::kForwardQueueSize);
  assert(manager.relayDiagnostics().queue_drops == 0);
  test_now = relay_config::kMaximumDelayMs;
  manager.update(false);
  assert(send_calls == 1 && last_tx_size == tlp::kRelayForwardPacketSize);
  tlp::RelayForwardPacket envelope{};
  tlp::PositionPacket original{};
  assert(tlp::deserializeRelayForwardPacket(last_tx, last_tx_size, &envelope,
                                             &original) ==
         tlp::RelayDecodeStatus::kOk);
  assert(original.source_device_id == 0x2222 &&
         original.sequence_number == 10);
}

void txDoneAndTimeoutRecovery() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);

  sendLocalPosition(manager, 7);
  const uint32_t before_done_rx = rx_calls;
  radio_state = RF_IDLE;  // DIO TX_DONE sets standby before the callback.
  terminal(false);
  assert(manager.isTransmitting());
  assert(rx_calls == before_done_rx);
  manager.update(false);
  assert(!manager.isTransmitting());
  assert(radio_state == RF_RX_RUNNING && rx_calls == before_done_rx + 1);

  sendLocalPosition(manager, 8);
  const uint32_t before_timeout_rx = rx_calls;
  fake_task = 2;
  radio_driver::acquire();
  callbacks->TxTimeout();
  assert(radio_state == RF_TX_RUNNING && rx_calls == before_timeout_rx);

  // Model SX126x-Arduino's nRF52 software timeout order: the application
  // callback returns first, then the dependency forces standby and sleep.
  radio_state = RF_IDLE; // Cleanup has exposed standby but is paused on BUSY.
  const auto calls_before = driver_calls;
  fake_task = 1;
  for (unsigned n = 0; n < 10; ++n) manager.update(false);
  uint8_t local[34]; makePosition(kLocalDevice, 9, local);
  assert(!manager.sendPositionPacket(local));
  assert(driver_calls == calls_before && manager.isTransmitting());
  fake_task = 2;
  standbyRadio();
  sleepRadio();
  radio_driver::release();
  fake_task = 1;
  manager.update(false);
  assert(!manager.isTransmitting() && manager.txTimeouts() == 1);
  assert(radio_state == RF_RX_RUNNING && rx_calls == before_timeout_rx + 1);
}

void duplicateMalformedAndBoundaries() {
  static_assert(tlp::kPositionPacketSize == 34, "POSITION wire size changed");
  static_assert(tlp::kRelayForwardPacketSize == 49,
                "RELAY_FORWARD wire size changed");

  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kBase);
  uint8_t packet[tlp::kPositionPacketSize];
  makePosition(0x3333, 22, packet);
  rxDone(packet, sizeof(packet), -80, 9);
  rxDone(packet, sizeof(packet), -90, 4);
  manager.update(false);
  assert(manager.baseDiagnostics().new_application_packets == 1);
  assert(manager.baseDiagnostics().duplicates == 1);
  assert(manager.baseDiagnostics().direct_observations == 2);

  uint8_t maximum[255]{};
  maximum[0] = tlp::kProtocolVersion;
  maximum[1] = 0xFE;
  rxDone(maximum, sizeof(maximum), -120, -10);
  rxDone(maximum, 256, -120, -10);
  manager.update(false);
  assert(manager.eventDiagnostics().oversized_rx_drops == 1);
  assert(manager.baseDiagnostics().new_application_packets == 1);
}

void roleSwitchDropsStaleRxAndIsolatesPendingTx() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kRelay);
  uint8_t packet[tlp::kPositionPacketSize];
  makePosition(0x4444, 31, packet);
  rxDone(packet, sizeof(packet), -99, 1);
  manager.setRole(NodeRole::kBase);
  manager.update(false);
  assert(manager.baseDiagnostics().direct_observations == 0);
  assert(manager.relayDiagnostics().queued == 0);
  assert(manager.eventDiagnostics().stale_rx_events == 1);

  manager.setRole(NodeRole::kRelay);
  manager.update(false);
  manager.update(false);
  makePosition(0x5555, 32, packet);
  rxDone(packet, sizeof(packet), -100, 2);
  manager.update(false);
  const uint32_t delay = deterministicRelayDelay(0x5555, 32, kLocalDevice);
  test_now += delay;
  manager.update(false);
  assert(send_calls == 1 && manager.isTransmitting());
  manager.setRole(NodeRole::kBase);
  radio_state = RF_IDLE;
  terminal(false);
  manager.update(false);
  assert(!manager.isTransmitting());
  assert(manager.relayDiagnostics().forwards_completed == 0);
}

void relayUsesSafeMonotonicClockAcrossCoreRollover() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kRelay);
  uint8_t packet[tlp::kPositionPacketSize];
  const uint64_t source = 0x6666;
  const uint32_t sequence = 77;
  makePosition(source, sequence, packet);
  test_now = UINT32_MAX - 100;
  rxDone(packet, sizeof(packet), -88, 7);
  manager.update(false);
  const uint32_t due = test_now +
      deterministicRelayDelay(source, sequence, kLocalDevice);
  test_now = due - 1;
  manager.update(false);
  assert(send_calls == 0);
  test_now = due;
  manager.update(false);
  assert(send_calls == 1 && last_tx_size == tlp::kRelayForwardPacketSize);
  tlp::RelayForwardPacket envelope{};
  tlp::PositionPacket original{};
  assert(tlp::deserializeRelayForwardPacket(last_tx, last_tx_size, &envelope,
                                             &original) ==
         tlp::RelayDecodeStatus::kOk);
  assert(original.source_device_id == source &&
         original.sequence_number == sequence);
}

void staleAndDuplicateTerminals(bool timeout_first) {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);
  sendLocalPosition(manager, 40);
  const auto a = radio_driver::generation();
  terminal(timeout_first);
  terminal(!timeout_first);
  assert(manager.eventDiagnostics().stale_tx_results == 1);
  manager.update(false);
  assert(manager.txTimeouts() == (timeout_first ? 1U : 0U));
  terminal(!timeout_first, a); // Old result between terminal and next TX.
  assert(manager.eventDiagnostics().stale_tx_results == 2);
  sendLocalPosition(manager, 41);
  const auto b = radio_driver::generation();
  assert(b != a);
  // Late identified A event cannot change either the current radio mode or B.
  const auto state = radio_state;
  terminal(true, a);
  assert(radio_state == state);
  manager.update(false);
  assert(manager.isTransmitting() && radio_driver::generation() == b);
  assert(manager.eventDiagnostics().stale_tx_results == 3);
  terminal(false);
  manager.update(false);
  assert(!manager.isTransmitting() && radio_state == RF_RX_RUNNING);
}

void roleQuiescenceWithDelayedDispatch() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kRelay);
  uint8_t packet[34]; makePosition(0x7777, 81, packet);
  fake_task = 2;
  radio_driver::acquire(); // Old IRQ decoded, callback not yet delivered.
  fake_task = 1;
  manager.setRole(NodeRole::kBase);
  const auto calls_before = driver_calls;
  manager.update(false);
  assert(manager.role() == NodeRole::kRelay && driver_calls == calls_before);
  fake_task = 2;
  callbacks->RxDone(packet, sizeof(packet), -100, 3);
  // Model a second DIO pending while the old dispatch is still in flight.
  memcpy(pending_packet, packet, sizeof(packet));
  pending_rx = true;
  IrqFired = true;
  radio_driver::release();
  fake_task = 1;
  rx_arrives_before_standby = true;
  manager.update(false);
  assert(manager.role() == NodeRole::kBase && radio_state == RF_RX_RUNNING);
  assert(manager.baseDiagnostics().direct_observations == 0);
  assert(manager.relayDiagnostics().queued == 0);
  assert(manager.eventDiagnostics().stale_rx_events == 2);
  assert(!pending_rx && !IrqFired); // Exact patched quiesce cleared both.
  IrqFired = true; // Late GPIO delivery after chip status was already cleared.
  RadioBgIrqProcess(); // Real patched wrapper: delayed semaphore wake.
  manager.update(false);
  assert(manager.baseDiagnostics().direct_observations == 0);
  rxDone(packet, sizeof(packet), -100, 3);
  manager.update(false);
  assert(manager.baseDiagnostics().new_application_packets == 1);
}

void ownerDeadlineAndQueueSaturation() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kTracker);
  test_now = UINT32_MAX - 100;
  send_cost_ms = 250; // Driver Send/BUSY time precedes watchdog arming.
  sendLocalPosition(manager, 90);
  uint8_t packet[34]; makePosition(0x8888, 90, packet);
  for (unsigned n = 0; n < 5; ++n) rxDone(packet, 34, -100, 3);
  assert(manager.eventDiagnostics().rx_queue_drops == 1);
  test_now += radio_config::kTxTimeoutMs - 1;
  manager.update(false);
  assert(manager.isTransmitting() && manager.txTimeouts() == 0);
  ++test_now;
  manager.update(false);
  assert(!manager.isTransmitting() && manager.txTimeouts() == 1);
  assert(radio_state == RF_RX_RUNNING && sleep_calls == 1);
}

void cancelledRoleRequestRetainsActiveForward() {
  RadioManager manager;
  TestSequence sequences;
  beginAs(manager, sequences, NodeRole::kRelay);
  uint8_t packet[34]; makePosition(0x9999, 91, packet);
  rxDone(packet, 34, -100, 3);
  manager.update(false);
  test_now = relay_config::kMaximumDelayMs;
  manager.update(false);
  assert(manager.isTransmitting());
  manager.setRole(NodeRole::kBase);
  terminal(false);
  manager.setRole(NodeRole::kRelay); // Latest request cancels, role never changed.
  manager.update(false);
  assert(manager.role() == NodeRole::kRelay && !manager.isTransmitting());
  assert(manager.relayDiagnostics().forwards_completed == 1);
  assert(radio_state == RF_RX_RUNNING);
}

void liveTxAdmissionAge() {
  TestSequence sequences;
  RadioManager manager;
  beginAs(manager, sequences, NodeRole::kTracker);
  uint8_t bytes[tlp::kPositionPacketSize];
  makePosition(kLocalDevice, 1, bytes);
  const uint32_t captured_at = UINT32_MAX - 3000;
  test_now = captured_at + gnss_config::kFreshFixMaxAgeMs;
  assert(!manager.sendPositionPacket(bytes, &captured_at));
  assert(send_calls == 0 && manager.canSend());
  --test_now;
  assert(manager.sendPositionPacket(bytes, &captured_at));
  assert(send_calls == 1);
}

void portableDeviceIdDiagnostics() {
  TestSequence sequences;
  RadioManager manager;
  beginAs(manager, sequences, NodeRole::kBase);
  // Expected strings are independent of the production split/printf logic.
  const struct { uint64_t id; const char* hex; } cases[] = {
      {0, "0000000000000000"},
      {0x00000000FFFFFFFFULL, "00000000FFFFFFFF"},
      {0xFFFFFFFF00000000ULL, "FFFFFFFF00000000"},
      {0x09A462BD4B275BA5ULL, "09A462BD4B275BA5"},
      {UINT64_MAX, "FFFFFFFFFFFFFFFF"}};
  for (const auto& value : cases) {
    uint8_t bytes[tlp::kTestPacketSize];
    assert(tlp::serializeTestPacket({value.id, UINT32_MAX, 123}, bytes, sizeof(bytes)));
    Serial.output.clear();
    rxDone(bytes, sizeof(bytes), -123, -17);
    manager.update(false);
    assert(Serial.output == std::string("RX source=") + value.hex +
           " sequence=4294967295 type=TEST RSSI=-123 dBm SNR=-17 dB\n");
  }

  beginAs(manager, sequences, NodeRole::kTracker);
  sendLocalPosition(manager, UINT32_MAX);
  assert(Serial.output.find("TX POSITION source=0102030405060708 sequence=4294967295 lat=410000000 lon=290000000 sats=9\n") != std::string::npos);

  constexpr uint64_t source = 0x89ABCDEF01234567ULL;
  uint8_t bytes[tlp::kPositionPacketSize];
  makePosition(source, 40, bytes);
  beginAs(manager, sequences, NodeRole::kRelay);
  rxDone(bytes, sizeof(bytes), -101, -8);
  manager.update(false);
  rxDone(bytes, sizeof(bytes), -101, -8);
  manager.update(false);
  assert(Serial.output.find("RELAY RX source=89ABCDEF01234567 seq=40 rssi=-101 snr=-8\n") != std::string::npos);
  assert(Serial.output.find("RELAY QUEUE source=89ABCDEF01234567 seq=40 delay=") != std::string::npos);
  assert(Serial.output.find("RELAY DUP source=89ABCDEF01234567 seq=40\n") != std::string::npos);
  for (uint32_t seq = 41; seq <= 44; ++seq) {
    makePosition(source, seq, bytes);
    rxDone(bytes, sizeof(bytes), -101, -8);
    manager.update(false);
  }
  assert(Serial.output.find("RELAY DROP queue-full source=89ABCDEF01234567 seq=44\n") != std::string::npos);
  test_now = relay_config::kMaximumDelayMs;
  manager.update(false);
  assert(Serial.output.find("RELAY TX source=89ABCDEF01234567 seq=40\n") != std::string::npos);

  beginAs(manager, sequences, NodeRole::kBase);
  makePosition(source, 40, bytes);
  rxDone(bytes, sizeof(bytes), -82, 6);
  manager.update(false);
  rxDone(bytes, sizeof(bytes), -83, 5);
  manager.update(false);
  assert(Serial.output.find("BASE RX NEW source=89ABCDEF01234567 seq=40 path=DIRECT rssi=-82 snr=6\n") != std::string::npos);
  assert(Serial.output.find("BASE RX DUP source=89ABCDEF01234567 seq=40 path=DIRECT rssi=-83 snr=5\n") != std::string::npos);
  tlp::RelayForwardPacket envelope{};
  envelope.relay_device_id = 0xFEDCBA9876543210ULL;
  envelope.ingress_rssi_dbm = -110;
  envelope.ingress_snr_db = -9;
  makePosition(source, 41, envelope.original_packet);
  uint8_t forwarded[tlp::kRelayForwardPacketSize];
  assert(tlp::serializeRelayForwardPacket(envelope, forwarded, sizeof(forwarded)));
  rxDone(forwarded, sizeof(forwarded), -82, 6);
  manager.update(false);
  assert(Serial.output.find("BASE RX NEW source=89ABCDEF01234567 seq=41 path=RELAY relay=FEDCBA9876543210 ingress_rssi=-110 ingress_snr=-9 rssi=-82 snr=6\n") != std::string::npos);

  beginAs(manager, sequences, NodeRole::kTracker);
  rxDone(bytes, sizeof(bytes), -82, 6);
  manager.update(false);
  assert(Serial.output.find("RX POSITION source=89ABCDEF01234567 seq=40 ignored role=TRACKER\n") != std::string::npos);
}

}  // namespace

namespace orun_tlp::monotonic {
uint32_t nowMs() { return test_now; }
}  // namespace orun_tlp::monotonic

const Radio_s Radio = {
    initRadio, getStatus, setChannel, setRxConfig, setTxConfig, send,
    sleepRadio, standbyRadio, receive, setSyncWord, getSyncWord};

void orunRadioDispatchLocked() {
  driverEntry();
  if (IrqFired.exchange(false) && pending_rx) {
    pending_rx = false;
    callbacks->RxDone(pending_packet, 34, -100, 3);
  }
  if (TimerTxTimeout) {
    TimerTxTimeout = false;
    callbacks->TxTimeout();
  }
}
void RadioStandby() { standbyRadio(); }
void RadioSleep() { sleepRadio(); }
void SX126xSetDioIrqParams(unsigned irq, unsigned dio1, unsigned dio2, unsigned dio3) {
  driverEntry();
  assert(radio_state == RF_IDLE && !irq && !dio1 && !dio2 && !dio3);
}
void SX126xClearIrqStatus(unsigned irq) {
  driverEntry();
  assert(radio_state == RF_IDLE && irq == IRQ_RADIO_ALL);
  pending_rx = false; // Hardware IRQ status is cleared while RX is stopped.
}

int lora_rak4630_init() { return 0; }

void BoardGetUniqueId(uint8_t* id) {
  for (uint8_t index = 0; index < 8; ++index)
    id[index] = static_cast<uint8_t>(kLocalDevice >> (56 - 8 * index));
}

int main() {
  portableDeviceIdDiagnostics();
  liveTxAdmissionAge();
  callbackOwnershipPayloadCopyAndOverflow();
  txDoneAndTimeoutRecovery();
  duplicateMalformedAndBoundaries();
  roleSwitchDropsStaleRxAndIsolatesPendingTx();
  relayUsesSafeMonotonicClockAcrossCoreRollover();
  staleAndDuplicateTerminals(false);
  staleAndDuplicateTerminals(true);
  roleQuiescenceWithDelayedDispatch();
  ownerDeadlineAndQueueSaturation();
  cancelledRoleRequestRetainsActiveForward();
  puts("R2 radio ownership, recovery and rollover checks: PASS");
}
