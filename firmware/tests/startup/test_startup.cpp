// Actual setup/loop, radio/gate, GNSS, PositionFlow and nRF journal backend.
// Only peripheral/RTOS APIs and the watchdog boundary are replaced on host.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <array>
#include <SX126x-Arduino.h>
#include <semphr.h>
#include <queue.h>
#include <nrf_sdm.h>
#include "radio_driver_gate.h"
#include "gnss_config.h"
#include "../../src/main.cpp"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;
constexpr uint64_t kHardwareId = 0x123456789ABCDEF0ULL;
unsigned erases = 0, programs = 0, rx_calls = 0, send_calls = 0;
unsigned watchdog_starts = 0, watchdog_feeds = 0, board_reads = 0;
uint32_t test_now = 0;
int lora_result = 0;
RadioEvents_t* callbacks = nullptr;
TestFicr ficr{kPageSize, 256};
TestUicr uicr{{0xF4000}};
TestFicr* NRF_FICR = &ficr;
TestUicr* NRF_UICR = &uicr;

uint32_t sd_softdevice_is_enabled(uint8_t* enabled) { *enabled = 0; return NRF_SUCCESS; }
uint32_t sd_flash_write(uint32_t* dst, const uint32_t* src, uint32_t count) {
  ++programs;
  const auto address = reinterpret_cast<uintptr_t>(dst);
  assert(address >= kBaseAddress && address + count * 4 <= kBaseAddress + kRegionSize);
  for (uint32_t i = 0; i < count; ++i) {
    assert(dst[i] == UINT32_MAX);
    dst[i] &= src[i];
  }
  return NRF_SUCCESS;
}
uint32_t sd_flash_page_erase(uint32_t page) {
  ++erases;
  assert(page >= kBaseAddress / kPageSize && page < (kBaseAddress + kRegionSize) / kPageSize);
  memset(reinterpret_cast<void*>(uintptr_t(page) * kPageSize), 0xFF, kPageSize);
  return NRF_SUCCESS;
}
// SoftDevice is always reported disabled above, so FlashMutationGate's
// pumpEvents()/pollPending() never reach sd_evt_get() here; this satisfies
// the link only. A dedicated M7P3 test exercises real event draining.
uint32_t sd_evt_get(uint32_t*) { return NRF_ERROR_NOT_FOUND; }

void BoardGetUniqueId(uint8_t* id) {
  ++board_reads;
  for (unsigned i = 0; i < 8; ++i) id[i] = uint8_t(kHardwareId >> (56 - 8 * i));
}
int lora_rak4630_init() { requireDriverGate(); return lora_result; }
void initRadio(RadioEvents_t* events) { requireDriverGate(); callbacks = events; }
RadioState_t getStatus() { requireDriverGate(); return RF_IDLE; }
void setChannel(uint32_t) { requireDriverGate(); }
void setRxConfig(RadioModems_t, uint32_t, uint32_t, uint8_t, uint32_t,
                 uint16_t, uint16_t, bool, uint8_t, bool, bool, uint8_t, bool, bool) { requireDriverGate(); }
void setTxConfig(RadioModems_t, int8_t, uint32_t, uint32_t, uint32_t, uint8_t,
                 uint16_t, bool, bool, bool, uint8_t, bool, uint32_t) { requireDriverGate(); }
void send(uint8_t*, uint8_t) { requireDriverGate(); ++send_calls; }
void stopRadio() { requireDriverGate(); }
void receive(uint32_t) { requireDriverGate(); ++rx_calls; }
void setSyncWord(uint16_t) { requireDriverGate(); }
uint16_t getSyncWord() { requireDriverGate(); return 0x1424; }
const Radio_s Radio = {initRadio, getStatus, setChannel, setRxConfig,
    setTxConfig, send, stopRadio, stopRadio, receive, setSyncWord, getSyncWord};
void orunRadioDispatchLocked() { requireDriverGate(); }
void orunRadioQuiesceLocked() { requireDriverGate(); }
void orunRadioTimeoutLocked() { requireDriverGate(); callbacks->TxTimeout(); }

uint32_t orun_tlp::monotonic::nowMs() { return test_now; }
void WatchdogManager::begin() { ++watchdog_starts; }
void WatchdogManager::feed() { ++watchdog_feeds; }
const WatchdogManager::BootInfo& WatchdogManager::bootInfo() {
  static const BootInfo info{};
  return info;
}

void settle(HistoryStore& store) {
  for (unsigned i = 0; i < 16 && store.busy(); ++i) store.poll();
  assert(store.ready() && !store.busy());
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string mode = argv[1];
  // "advfail"/"blefail" keep the radio healthy and instead fail BLE:
  // Advertising.start() returning false / Bluefruit.begin() returning false.
  const bool ble_advertising_fails = mode == "advfail";
  const bool ble_runtime_fails = mode == "blefail";
  // "noevent" is a negative control: same healthy boot, but Bluefruit's global
  // event callback never fires, proving the short-cycle assertions below fail
  // without the direct event handoff.
  const bool no_event_control = mode == "noevent";
  const bool success = mode == "success" || ble_advertising_fails || ble_runtime_fails ||
                       no_event_control;
  // Each scenario runs in a new process, like a cold boot (static driver gate).
  assert(success || mode == "mutex" || mode == "gate" || mode == "queue" ||
         mode == "lora");
  void* region = mmap(reinterpret_cast<void*>(kBaseAddress), kRegionSize,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(region == reinterpret_cast<void*>(kBaseAddress));
  memset(region, 0xFF, kRegionSize);
  // M7P5: setup() also begins ConfigStore, backed by NrfConfigFlash over the
  // M7P1-decided config partition -- map it too, exactly like history above.
  constexpr uint32_t kConfigRegionSize = kFutureConfigRegionEnd - kFutureConfigRegionStart;
  void* config_region = mmap(reinterpret_cast<void*>(kFutureConfigRegionStart), kConfigRegionSize,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(config_region == reinterpret_cast<void*>(kFutureConfigRegionStart));
  memset(config_region, 0xFF, kConfigRegionSize);
  // M7P6B: setup() also begins SecurityStore (recovery only), backed by
  // NrfSecurityFlash over the M7P1-decided security partition -- map it too.
  constexpr uint32_t kSecurityRegionSize = kFutureSecurityRegionEnd - kFutureSecurityRegionStart;
  void* security_region = mmap(reinterpret_cast<void*>(kFutureSecurityRegionStart), kSecurityRegionSize,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(security_region == reinterpret_cast<void*>(kFutureSecurityRegionStart));
  memset(security_region, 0xFF, kSecurityRegionSize);

  // Seed page 0 through the real journal/backend, including a committed fix.
  NrfHistoryFlash seed_flash;
  HistoryStore seed(seed_flash);
  assert(seed.begin(kHardwareId));
  settle(seed);
  HistoryStore::Record original{};
  uint32_t sequence;
  assert(seed.nextSequence(sequence, original.identity));
  const tlp::PositionPacket packet{kHardwareId, sequence, 0,
      410000000, 290000000, 10, 100, 8, tlp::kPositionFlagValidFix};
  assert(tlp::serializePositionPacket(packet, original.packet, sizeof(original.packet)));
  assert(seed.append(original.packet, original.identity));
  settle(seed);
  std::array<uint8_t, kRegionSize> before{};
  memcpy(before.data(), region, before.size());
  erases = programs = 0;

  fake_mutex_create_failure = mode == "mutex";
  fake_mutex_take_failure = mode == "gate";
  fake_queue_create_failure = mode == "queue";
  lora_result = mode == "lora" ? -1 : 0;
  Bluefruit.Advertising.start_result = !ble_advertising_fails;
  Bluefruit.begin_result = !ble_runtime_fails;
  setup();
  assert(board_reads == 1 && radio_manager.deviceId() == kHardwareId);
  assert(watchdog_starts == 1);
  assert(history.ready() && history.count() == 1);
  assert(history.diagnostics().recovery_corruptions == 0);
  assert(erases == 0 && programs == 0);
  assert(memcmp(region, before.data(), before.size()) == 0);
  // BLE boot path: ready reflects Bluefruit.begin(); "available"/admission
  // only follow a successful Advertising.start(0), which is called exactly
  // once and only after begin() succeeded.
  const bool ble_ok = !ble_advertising_fails && !ble_runtime_fails;
  assert(ble_ready == !ble_runtime_fails);
  // M7P7G: the application GATT service is created only after a successful
  // Bluefruit runtime start, with the frozen request/response properties and
  // 20-byte ATT-frame ceiling. This is composition evidence, not physical BLE.
  assert(ble_application_gatt_ready == !ble_runtime_fails);
  if (!ble_runtime_fails) {
    assert(ble_application_request_characteristic.properties == CHR_PROPS_WRITE);
    assert(ble_application_request_characteristic.max_len ==
           orun_tlp::ble_app_transport::kMaxFrameSize);
    assert(ble_application_response_characteristic.properties ==
           CHR_PROPS_INDICATE);
    assert(ble_application_response_characteristic.max_len ==
           orun_tlp::ble_app_transport::kMaxFrameSize);
    assert(ble_application_response_value_handle != BLE_GATT_HANDLE_INVALID);
  }
  assert(Bluefruit.Advertising.start_calls == (ble_runtime_fails ? 0U : 1U));
  assert(ble_admission.isOpen() == ble_ok);
  assert((Serial.output.find("BLE available name=ORUN-") != std::string::npos) == ble_ok);
  assert((Serial.output.find("BLE advertising start failed\n") != std::string::npos) ==
         ble_advertising_fails);
  assert((Serial.output.find("BLE unavailable\n") != std::string::npos) == ble_runtime_fails);
  assert(ble_initial_start ==
         (ble_runtime_fails ? BleInitialStart::kNotAttempted
          : ble_advertising_fails ? BleInitialStart::kFail : BleInitialStart::kOk));
  const bool diagnostic = Serial.output.find("RADIO unavailable; TX/RX disabled; local services continue") != std::string::npos;
  assert(diagnostic == !success); // Executes main's handling of begin(false).
  assert(radio_manager.canSend() == success);
  assert((rx_calls != 0) == success && send_calls == 0);

  // Complete recovery and GNSS detection/configuration through the actual loop.
  for (unsigned i = 0; i < 16; ++i) {
    // Wait for both rail transitions, then service the loop at normal cadence.
    test_now += i < 2 ? gnss_config::kPowerSettleMs : 10;
    loop();
  }
  assert(watchdog_feeds == 16 && fake_idle_calls == 16);
  assert(gnss_manager.state() == GnssManager::State::kAcquiring);
  assert(role_controller.role() == NodeRole::kTracker);
  assert(erases == 0 && programs == 2); // Reservation only; no new page/erase.
  assert(memcmp(region, before.data(), journal_format::kStaticHeaderSize) == 0);
  assert(memcmp(static_cast<uint8_t*>(region) + kPageHeaderSize,
                before.data() + kPageHeaderSize, kRegionSize - kPageHeaderSize) == 0);
  HistoryStore::Record recovered{};
  assert(history.lookup(original.identity, recovered));
  assert(memcmp(recovered.packet, original.packet, sizeof(original.packet)) == 0);

  // Feed a fresh matched PVT/DOP through production GNSS -> store-first flow.
  using Fake = SFE_UBLOX_GNSS;
  for (uint32_t tow : {1000U, 2000U}) {
    Fake::pending.push_back([tow] {
      UBX_NAV_PVT_data_t pvt{};
      pvt.iTOW = tow; pvt.flags.bits.gnssFixOK = true; pvt.fixType = 3;
      pvt.lat = 410000001; pvt.lon = 290000001; pvt.numSV = 8;
      Fake::current_pvt = pvt; Fake::itow_fresh = true;
      Fake::pvt(&pvt);
      UBX_NAV_DOP_data_t dop{tow, 100};
      Fake::dop(&dop);
    });
    loop(); // First epoch establishes the R3 boundary; second is fresh.
  }
  loop();
  assert(history.count() == 2 && erases == 0 && programs == 4);
  assert(history.newest(recovered) && recovered.identity > original.identity);
  tlp::PositionPacket newest{};
  assert(tlp::deserializePositionPacket(recovered.packet, sizeof(recovered.packet), &newest));
  assert(newest.source_device_id == kHardwareId && newest.latitude_e7 == 410000001);
  assert(send_calls == (success ? 1U : 0U));
  assert(radio_manager.isTransmitting() == success);
  if (!success) {
    assert(rx_calls == 0);
    assert(!radio_manager.sendPositionPacket(recovered.packet));
    assert(send_calls == 0);
  }
  assert(watchdog_feeds == 19 && fake_idle_calls == 19);

  // The USB diagnostic must be queryable after the early boot window is gone.
  // While the bounded probe is incomplete it reports PENDING, not ABSENT.
  Serial.output.clear();
  Serial.queueInput("ACCEL?\n");
  pollRoleCommands();
  assert(Serial.output == "ACCEL PENDING\n");

  Serial.output.clear();
  Serial.queueInput("ACTIVITY START\nACTIVITY?\n");
  while (Serial.available()) pollRoleCommands();
  assert(Serial.output == "ACTIVITY START rejected: PENDING\n"
                          "ACTIVITY UNAVAILABLE accel=PENDING\n");

  // The startup Wire stub has no LIS3DH response. Advance exactly to the third
  // bounded detection attempt, latch the real manager event, then prove the same
  // query returns the retained result without re-probing hardware.
  test_now = 2250;
  handleAccelerometerEvent(accelerometer_manager.poll(test_now));
  assert(accelerometer_manager.detectionComplete());
  assert(!accelerometer_manager.detected());
  Serial.output.clear();
  Serial.queueInput("ACCEL?\n");
  pollRoleCommands();
  assert(Serial.output == "ACCEL ABSENT\n");

  // Additive M6B3 parser checks use the actual main.cpp serial surface. Queries
  // and rejected starts must not transact on Wire or alter role/config.
  const auto wire_calls = Wire.transaction_calls;
  const auto prior_role = role_controller.role();
  Serial.output.clear();
  Serial.queueInput("ACTIVITY STA");
  pollRoleCommands();
  assert(Serial.output.empty());
  Serial.queueInput("RT\r\nACTIVITY?\rACTIVITY?\n");
  while (Serial.available()) pollRoleCommands();
  assert(Serial.output == "ACTIVITY START rejected: ABSENT\n"
                          "ACTIVITY UNAVAILABLE accel=ABSENT\n"
                          "ACTIVITY UNAVAILABLE accel=ABSENT\n");
  assert(activity_capture.state() == ActivityCapture::State::kIdle);
  assert(role_controller.role() == prior_role);
  assert(Wire.transaction_calls == wire_calls);
  Serial.output.clear();
  Serial.queueInput("ACTIVITY STARTxxxxxxxxxxxxxxxxxxxxxxxx\nACTIVITY STARTX\nACCEL?\n");
  while (Serial.available()) pollRoleCommands();
  assert(Serial.output == "ROLE command rejected: too long\n"
                          "ROLE command rejected\nACCEL ABSENT\n");
  assert(Wire.transaction_calls == wire_calls);
  Serial.output.clear();
  Serial.queueInput("ROLE RELAY\rROLE?\nROLE BASE\r\nROLE TRACKER\nROLE?\n");
  while (Serial.available()) pollRoleCommands();
  assert(Serial.output == "ROLE RELAY source=USB-OVERRIDE\n"
                          "ROLE RELAY mode=OVERRIDE\n"
                          "ROLE BASE source=USB-OVERRIDE\n"
                          "ROLE TRACKER source=USB-OVERRIDE\n"
                          "ROLE TRACKER mode=OVERRIDE\n");
  assert(Wire.transaction_calls == wire_calls);

  // M7P7D: USB is only an adapter into the typed application request seam.
  // APP CONFIG? is intentionally read-only and must not touch flash/radio/role.
  // Drive the REAL production loop here: deleting drainApplicationResponse()
  // from loop() must make this test fail rather than letting a manual drain
  // create a false-positive composition test.
  const unsigned app_query_programs = programs;
  const unsigned app_query_erases = erases;
  Serial.output.clear();
  Serial.queueInput("APP CONFIG?\n");
  loop();
  assert(!application_requests.responsePending());
  assert(Serial.output.find(
             "APP RESULT id=1 code=OK config_backend_ready=yes source=default "
             "tracking_interval_seconds=180 battery_capacity_mah=0\n") !=
         std::string::npos);
  assert(programs == app_query_programs && erases == app_query_erases);
  assert(role_controller.role() == prior_role);
  assert(Wire.transaction_calls == wire_calls);

  // The one-result slot provides bounded backpressure. Two commands fit inside
  // the production parser's per-loop byte budget: first is accepted, second is
  // BUSY, then loop() drains only the accepted result. Rejected work receives
  // no correlation id.
  Serial.output.clear();
  Serial.queueInput("APP CONFIG?\nAPP CONFIG?\n");
  loop();
  assert(!application_requests.responsePending());
  const size_t busy_pos = Serial.output.find("APP BUSY\n");
  const size_t result_pos = Serial.output.find(
      "APP RESULT id=2 code=OK config_backend_ready=yes source=default "
      "tracking_interval_seconds=180 battery_capacity_mah=0\n");
  assert(busy_pos != std::string::npos);
  assert(result_pos != std::string::npos && busy_pos < result_pos);
  assert(programs == app_query_programs && erases == app_query_erases);

  // M7P7E composition: a future BLE-owned response must not be consumed by
  // the production USB drain. While that result is pending, APP CONFIG? is
  // BUSY and does not consume a USB request id; owner-only cleanup restores
  // the bounded slot.
  assert(application_requests.submit(orun_tlp::ApplicationRequest{
             orun_tlp::ApplicationRequester::kBle, 900,
             orun_tlp::ApplicationRequestKind::kGetConfig}) ==
         orun_tlp::ApplicationSubmitResult::kAccepted);
  const uint32_t usb_id_before_ble_pending = next_usb_application_request_id;
  Serial.output.clear();
  Serial.queueInput("APP CONFIG?\n");
  loop();
  assert(Serial.output.find("APP BUSY\n") != std::string::npos);
  assert(application_requests.responsePending());
  assert(next_usb_application_request_id == usb_id_before_ble_pending);
  assert(application_requests.discardResponse(
      orun_tlp::ApplicationRequester::kBle));
  assert(!application_requests.responsePending());
  assert(programs == app_query_programs && erases == app_query_erases);

  // Parser negatives remain ordinary rejected role/diagnostic commands and do
  // not accidentally alias the application query.
  Serial.output.clear();
  Serial.queueInput("APP CONFIG\nAPP CONFIG??\napp config?\n");
  while (Serial.available()) pollRoleCommands();
  assert(Serial.output == "ROLE command rejected\n"
                          "ROLE command rejected\n"
                          "ROLE command rejected\n");
  assert(!application_requests.responsePending());

  // Local diagnostic request-id wrap skips zero and stays a correlation aid
  // only; it is not a protocol/security/message identity.
  next_usb_application_request_id = UINT32_MAX;
  Serial.output.clear();
  Serial.queueInput("APP CONFIG?\n");
  loop();
  assert(!application_requests.responsePending());
  assert(next_usb_application_request_id == 1);
  assert(Serial.output.find(
             "APP RESULT id=4294967295 code=OK config_backend_ready=yes "
             "source=default tracking_interval_seconds=180 "
             "battery_capacity_mah=0\n") != std::string::npos);
  assert(programs == app_query_programs && erases == app_query_erases);

  // BLE? reports runtime readiness, advertising state, connection count,
  // admission policy and the boot-time start result as separate facts.
  const char* expected_ble =
      ble_runtime_fails
          ? "BLE ready=no advertising=no connected=0 policy=closed initial_start=not-attempted\n"
      : ble_advertising_fails
          ? "BLE ready=yes advertising=no connected=0 policy=closed initial_start=fail\n"
          : "BLE ready=yes advertising=yes connected=0 policy=open initial_start=ok\n";
  Serial.output.clear();
  Serial.queueInput("BLE?\n");
  pollRoleCommands();
  assert(Serial.output == expected_ble);
  // Query is repeatable and leaves no residual command bytes behind.
  Serial.output.clear();
  Serial.queueInput("BLE?\rBLE\nBLE??\nBLE?\n");
  while (Serial.available()) pollRoleCommands();
  assert(Serial.output == std::string(expected_ble) +
                          "ROLE command rejected\n"
                          "ROLE command rejected\n" + expected_ble);

  // Production explicitly owns restart: the framework's own (result-ignoring)
  // restart-on-disconnect must be off whenever BLE runtime is up. The
  // disconnect handoff must use Bluefruit's direct global event callback, NOT
  // Periph's ada_callback-routed (heap-allocating, droppable) disconnect
  // callback.
  if (!ble_runtime_fails) {
    assert(!Bluefruit.Advertising.restart_on_disconnect);
    assert(Bluefruit.event_cb == &onBleEvent);
    assert(Bluefruit.Periph.disconnect_cb == nullptr);
  }

  auto bleQuery = [&]() {
    Serial.output.clear();
    Serial.queueInput("BLE?\n");
    pollRoleCommands();
    return Serial.output;
  };
  auto tick = [&](uint32_t advance_ms) {
    test_now += advance_ms;
    Serial.output.clear();
    loop();
    // Only BLE lines: other subsystems log on large clock jumps.
    std::string ble_lines;
    for (size_t pos = 0; pos < Serial.output.size();) {
      size_t end = Serial.output.find('\n', pos);
      end = end == std::string::npos ? Serial.output.size() : end + 1;
      if (Serial.output.compare(pos, 4, "BLE ") == 0) ble_lines += Serial.output.substr(pos, end - pos);
      pos = end;
    }
    return ble_lines;
  };
  auto has = [](const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
  };
  const uint32_t kWindow = ble_admission_config::kNoClientTimeoutMs;
  const uint32_t kRetry = ble_admission_config::kRetryIntervalMs;

  if (!ble_ok) {
    // Fail-closed boot: many loop ticks later still no admission window, no
    // start attempt beyond the boot one, no close, no false "available".
    const unsigned starts = Bluefruit.Advertising.start_calls;
    for (int i = 0; i < 5; ++i) {
      const std::string out = tick(kWindow);
      assert(out.empty());
    }
    assert(Bluefruit.Advertising.start_calls == starts);
    assert(Bluefruit.Advertising.stop_calls == 0);
    assert(!ble_admission.isOpen());
  }

  if (no_event_control) {
    // Negative control: identical short connect+disconnect as the main flow
    // below, but the event never reaches onBleEvent(). The fresh window is
    // then NOT granted and the original deadline closes BLE. (The main flow
    // asserts the opposite, so it fails if registration/delivery is lost.)
    Bluefruit.event_delivery = false;
    tick(kWindow - 5000);
    Bluefruit.simulateConnect();
    Bluefruit.simulateDisconnect();  // Entirely between two loop polls.
    assert(Bluefruit.event_cb_calls == 0 && ble_disconnect_events == 0);
    tick(1000);
    assert(has(tick(6000), "BLE closed; no client connected within window\n"));
  }

  if (ble_ok && !no_event_control) {
    // --- Callback isolation (constraint: no loop-only work off-task). ------
    {
      const unsigned enters = critical_entries;
      const uint32_t clock_before = test_now;
      const uint32_t events_before = ble_disconnect_events;
      Serial.output.clear();
      ble_evt_t disconnect_evt{};
      disconnect_evt.header.evt_id = BLE_GAP_EVT_DISCONNECTED;
      onBleEvent(&disconnect_evt);
      // Exactly one critical section, balanced, one counter increment, and
      // nothing else observable: no log, no policy change, no Bluefruit call.
      assert(critical_entries == enters + 1 && critical_depth == 0);
      assert(ble_disconnect_events == events_before + 1);
      assert(Serial.output.empty());
      assert(ble_admission.isOpen() && !ble_admission.isConnected());
      assert(Bluefruit.Advertising.start_calls == 1 && Bluefruit.Advertising.stop_calls == 0);
      assert(test_now == clock_before);
      // Any other event id (connect, conn-param update, ...) is ignored
      // entirely: no critical section, no counter change.
      for (uint16_t id : {BLE_GAP_EVT_CONNECTED, uint16_t{0x12}, uint16_t{0x50}}) {
        ble_evt_t other{};
        other.header.evt_id = id;
        onBleEvent(&other);
      }
      assert(critical_entries == enters + 1 && critical_depth == 0);
      assert(ble_disconnect_events == events_before + 1);
      assert(Serial.output.empty() && test_now == clock_before);
      // Loop consumes it as one (harmless, still-open) event; nothing else.
      const std::string out = tick(0);
      assert(out.empty());
      assert(ble_disconnect_events_seen == ble_disconnect_events);
      assert(tick(0).empty());
    }

    // --- Connected client: framework stops advertising on connect; policy
    // stays open, never closes while connected, never tries to restart. -----
    Bluefruit.simulateConnect();
    assert(tick(kWindow + 1000).empty());
    assert(bleQuery() ==
           "BLE ready=yes advertising=no connected=1 policy=open initial_start=ok\n");
    assert(Bluefruit.Advertising.start_calls == 1 && Bluefruit.Advertising.stop_calls == 0);

    // --- Real production GATT path: WRITE -> loop dispatch -> non-blocking
    // HVX -> exact HVC. This closes the previous host gap where startup only
    // asserted GATT object composition without exercising indication state.
    ble_application_response_characteristic.indicate_enabled = true;
    const uint8_t get_config_1[] = {
        0x01, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00};
    const unsigned hvx_before = BluefruitHvx.calls;
    BluefruitHvx.result = NRF_SUCCESS;
    ble_application_request_characteristic.simulateWrite(
        Bluefruit.connHandle(), get_config_1, sizeof(get_config_1));
    assert(tick(0).empty());
    assert(BluefruitHvx.calls == hvx_before + 1);
    assert(BluefruitHvx.conn_handle == Bluefruit.connHandle());
    assert(BluefruitHvx.value_handle == ble_application_response_value_handle);
    assert(BluefruitHvx.type == BLE_GATT_HVX_INDICATION);
    assert(BluefruitHvx.len == 18);
    assert(BluefruitHvx.data[0] == 0x01 && BluefruitHvx.data[1] == 0x81);
    assert(BluefruitHvx.data[4] == 0x01 && BluefruitHvx.data[5] == 0x00);
    assert(ble_application_indication_in_flight);
    assert(ble_application_transport.outboundFramePending());
    Bluefruit.simulateHvc(ble_application_response_value_handle);
    assert(tick(0).empty());
    assert(!ble_application_indication_in_flight);
    assert(!ble_application_transport.outboundFramePending());

    // A documented transient HVX return is retried at bounded spacing with
    // the same pending response, without disconnecting or reopening ingress.
    const uint8_t get_config_2[] = {
        0x01, 0x01, 0x03, 0x00, 0x02, 0x00, 0x00, 0x00};
    BluefruitHvx.result = NRF_ERROR_BUSY;
    const unsigned transient_hvx_before = BluefruitHvx.calls;
    ble_application_request_characteristic.simulateWrite(
        Bluefruit.connHandle(), get_config_2, sizeof(get_config_2));
    assert(tick(0).empty());
    assert(BluefruitHvx.calls == transient_hvx_before + 1);
    assert(!ble_application_indication_in_flight);
    assert(ble_application_transport.outboundFramePending());
    assert(!ble_application_handoff.ingressAllowed());
    assert(!ble_application_disconnect_pending);
    assert(tick(kBleApplicationIndicationRetryMs - 1).empty());
    assert(BluefruitHvx.calls == transient_hvx_before + 1);
    BluefruitHvx.result = NRF_SUCCESS;
    assert(tick(1).empty());
    assert(BluefruitHvx.calls == transient_hvx_before + 2);
    assert(ble_application_indication_in_flight);
    Bluefruit.simulateHvc(ble_application_response_value_handle);
    assert(tick(0).empty());
    assert(!ble_application_transport.outboundFramePending());

    // --- Disconnect: framework does NOT restart (restartOnDisconnect(false));
    // the direct event callback counts it; loop restarts and grants a window.
    Bluefruit.simulateDisconnect();
    assert(Bluefruit.Periph.pending_disconnect_cbs == 0);  // no Periph/ada path
    assert(!Bluefruit.Advertising.isRunning());  // no split ownership
    assert(has(tick(0), "BLE advertising restarted\n"));
    assert(Bluefruit.Advertising.start_calls == 2);
    assert(bleQuery() ==
           "BLE ready=yes advertising=yes connected=0 policy=open initial_start=ok\n");
    // Polled-edge + event for the same disconnect: no second start.
    assert(tick(1).empty() && Bluefruit.Advertising.start_calls == 2);

    // --- Terminal HVX SVC return: S140 can report a protocol timeout
    // directly from sd_ble_gatts_hvx(). That return must take the same
    // terminal cleanup/disconnect path even if no timeout event handoff is
    // available to rescue the session.
    Bluefruit.simulateConnect();
    tick(0);
    assert(ble_application_session_active);
    ble_application_response_characteristic.indicate_enabled = true;
    const uint8_t get_config_3[] = {
        0x01, 0x01, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00};
    BluefruitHvx.result = NRF_ERROR_TIMEOUT;
    Bluefruit.disconnect_result = false;
    const unsigned terminal_disconnect_calls = Bluefruit.disconnect_calls;
    ble_application_request_characteristic.simulateWrite(
        Bluefruit.connHandle(), get_config_3, sizeof(get_config_3));
    assert(has(tick(0), "BLE indication submit terminal error="));
    assert(!ble_application_session_active);
    assert(!ble_application_handoff.sessionActive());
    assert(!ble_application_transport.outboundFramePending());
    assert(ble_application_disconnect_pending);
    assert(Bluefruit.Periph.connected() == 1);
    assert(Bluefruit.disconnect_calls == terminal_disconnect_calls + 1);
    // Existing 1 s recovery cadence applies; terminal submission failure must
    // not create a fast retry loop or a fresh app session on the dead link.
    assert(tick(kBleApplicationDisconnectRetryMs - 1).empty());
    assert(Bluefruit.disconnect_calls == terminal_disconnect_calls + 1);
    assert(!ble_application_session_active);
    Bluefruit.disconnect_result = true;
    BluefruitHvx.result = NRF_SUCCESS;
    assert(tick(1).empty());
    assert(Bluefruit.disconnect_calls == terminal_disconnect_calls + 2);
    assert(Bluefruit.Periph.connected() == 0);
    assert(ble_application_disconnect_pending);
    assert(has(tick(0), "BLE advertising restarted\n"));
    assert(!ble_application_disconnect_pending);

    // --- GATTS protocol timeout: callback only hands off the terminal fact;
    // loop tears down the app session and retries physical disconnect without
    // ever reopening a fresh session on the dead ATT link. ------------------
    Bluefruit.simulateConnect();
    tick(0);  // admits the application session
    assert(ble_application_session_active);
    assert(ble_application_handoff.sessionActive());
    const unsigned timeout_enters = critical_entries;
    const unsigned disconnect_calls_before = Bluefruit.disconnect_calls;
    const uint32_t timeout_clock_before = test_now;
    Bluefruit.disconnect_result = false;
    Serial.output.clear();
    Bluefruit.simulateGattTimeout();
    assert(critical_entries == timeout_enters + 1 && critical_depth == 0);
    assert(Serial.output.empty());
    assert(test_now == timeout_clock_before);
    assert(Bluefruit.disconnect_calls == disconnect_calls_before);
    assert(!ble_application_handoff.ingressAllowed());

    assert(has(tick(0), "BLE GATT protocol timeout; disconnecting\n"));
    assert(!ble_application_session_active);
    assert(ble_application_disconnect_pending);
    assert(Bluefruit.Periph.connected() == 1);
    assert(Bluefruit.disconnect_calls == disconnect_calls_before + 1);

    // Failed disconnect request must not busy-spin or recreate an ORUN
    // application session on the timed-out ATT connection.
    assert(tick(kBleApplicationDisconnectRetryMs - 1).empty());
    assert(Bluefruit.disconnect_calls == disconnect_calls_before + 1);
    assert(!ble_application_session_active);
    Bluefruit.disconnect_result = true;
    assert(tick(1).empty());
    assert(Bluefruit.disconnect_calls == disconnect_calls_before + 2);
    assert(Bluefruit.Periph.connected() == 0);
    assert(ble_application_disconnect_pending);  // edge consumed next tick
    assert(has(tick(0), "BLE advertising restarted\n"));
    assert(!ble_application_disconnect_pending);
    assert(!ble_application_session_active);

    // --- Post-disconnect start FAILS, then a later retry succeeds. ---------
    Bluefruit.simulateConnect();
    tick(1000);
    Bluefruit.simulateDisconnect();
    Bluefruit.Advertising.start_result = false;
    unsigned starts_before = Bluefruit.Advertising.start_calls;
    assert(has(tick(0), "BLE advertising restart failed; retrying\n"));
    assert(Bluefruit.Advertising.start_calls == starts_before + 1);
    // Truthful state: not advertising, policy still open (window not over).
    assert(bleQuery() ==
           "BLE ready=yes advertising=no connected=0 policy=open initial_start=ok\n");
    // No busy-spin: ticks inside the retry interval do not call start().
    assert(tick(0).empty() && tick(kRetry - 1).empty());
    assert(Bluefruit.Advertising.start_calls == starts_before + 1);
    // Retry due, still failing: one more attempt, no log spam.
    assert(tick(1).empty());
    assert(Bluefruit.Advertising.start_calls == starts_before + 2);
    // Later retry succeeds.
    Bluefruit.Advertising.start_result = true;
    assert(tick(kRetry).empty() == false);  // "restarted" logged
    assert(Bluefruit.Advertising.start_calls == starts_before + 3);
    assert(bleQuery() ==
           "BLE ready=yes advertising=yes connected=0 policy=open initial_start=ok\n");

    // --- Short connect+disconnect FULLY missed by polling. ------------------
    // Old window's deadline is about to pass; the missed session's real
    // disconnect must grant a fresh window anyway.
    tick(kWindow - 5000);
    assert(bleQuery() ==
           "BLE ready=yes advertising=yes connected=0 policy=open initial_start=ok\n");
    Bluefruit.simulateConnect();
    Bluefruit.simulateDisconnect();  // Same framework tick: no poll saw it.
    Bluefruit.Advertising.running = false;  // Model restart owner: loop only.
    // Delivered through the direct global event callback (no Periph/ada_callback
    // path exists): the counter moved without any loop tick or deferred task.
    assert(Bluefruit.event_cb_calls >= 1 && Bluefruit.Periph.pending_disconnect_cbs == 0);
    assert(ble_disconnect_events != ble_disconnect_events_seen);
    assert(Bluefruit.Periph.connected() == 0);
    assert(has(tick(1000), "BLE advertising restarted\n"));
    // Past the ORIGINAL deadline: without the event this would have closed.
    assert(!has(tick(6000), "BLE closed"));
    assert(bleQuery() ==
           "BLE ready=yes advertising=yes connected=0 policy=open initial_start=ok\n");

    // --- Connection racing the close: client wins, session stays admitted. --
    tick(kWindow - 6000 - 1);  // just before the fresh window ends
    assert(!has(tick(0), "BLE closed"));
    static bool raced;
    raced = false;
    Bluefruit.Advertising.stop_race = [] { Bluefruit.simulateConnect(); raced = true; };
    starts_before = Bluefruit.Advertising.start_calls;
    assert(!has(tick(1), "BLE closed"));  // stop() failed: no false log
    assert(raced);
    Bluefruit.Advertising.stop_race = nullptr;
    // Physical truth right after the race, policy not closed, not restarted.
    assert(bleQuery() ==
           "BLE ready=yes advertising=no connected=1 policy=closing initial_start=ok\n");
    assert(!has(tick(0), "BLE closed"));
    assert(bleQuery() ==
           "BLE ready=yes advertising=no connected=1 policy=open initial_start=ok\n");
    // Far past any window while connected: no close, no stop, no start.
    const unsigned stops = Bluefruit.Advertising.stop_calls;
    assert(!has(tick(kWindow * 3), "BLE closed"));
    assert(Bluefruit.Advertising.stop_calls == stops);
    assert(Bluefruit.Advertising.start_calls == starts_before);
    // Real disconnect afterwards: restart + fresh window.
    Bluefruit.simulateDisconnect();
    assert(has(tick(0), "BLE advertising restarted\n"));
    assert(bleQuery() ==
           "BLE ready=yes advertising=yes connected=0 policy=open initial_start=ok\n");
    assert(!has(tick(kWindow - 1), "BLE closed"));

    // --- A whole connect+disconnect completing between stop() and the loop's
    // state reads at the deadline: neither read shows the client, but the real
    // disconnect (counted by the event callback) is owed a fresh window. ------
    Bluefruit.Advertising.stop_race = [] {
      Bluefruit.simulateConnect();
      Bluefruit.simulateDisconnect();
    };
    assert(!has(tick(1), "BLE closed"));  // close NOT confirmed
    Bluefruit.Advertising.stop_race = nullptr;
    assert(Bluefruit.Periph.connected() == 0 && !Bluefruit.Advertising.isRunning());
    assert(bleQuery() ==
           "BLE ready=yes advertising=no connected=0 policy=closing initial_start=ok\n");
    assert(has(tick(0), "BLE advertising restarted\n"));
    assert(bleQuery() ==
           "BLE ready=yes advertising=yes connected=0 policy=open initial_start=ok\n");
    assert(!has(tick(kWindow - 1), "BLE closed"));

    // --- Final expiry, stop() failing first, then succeeding. ---------------
    Bluefruit.Advertising.stop_result = false;
    const unsigned stop_base = Bluefruit.Advertising.stop_calls;
    assert(!has(tick(1), "BLE closed"));  // requested, NOT confirmed
    assert(Bluefruit.Advertising.stop_calls == stop_base + 1);
    // Truthful: still advertising, policy closing (not closed).
    assert(bleQuery() ==
           "BLE ready=yes advertising=yes connected=0 policy=closing initial_start=ok\n");
    // Throttled retry, never busy-spins, never logs a false close.
    assert(!has(tick(0), "BLE closed") && !has(tick(kRetry - 1), "BLE closed"));
    assert(Bluefruit.Advertising.stop_calls == stop_base + 1);
    assert(!has(tick(1), "BLE closed"));
    assert(Bluefruit.Advertising.stop_calls == stop_base + 2);
    // stop() finally works: physical close confirmed, only now logged.
    Bluefruit.Advertising.stop_result = true;
    assert(has(tick(kRetry), "BLE closed; no client connected within window\n"));
    assert(bleQuery() ==
           "BLE ready=yes advertising=no connected=0 policy=closed initial_start=ok\n");
    // Stays closed: no more stop/start attempts, no reopen from events.
    const unsigned stops_done = Bluefruit.Advertising.stop_calls;
    const unsigned starts_done = Bluefruit.Advertising.start_calls;
    {
      ble_evt_t late{};
      late.header.evt_id = BLE_GAP_EVT_DISCONNECTED;
      onBleEvent(&late);
    }
    assert(tick(kWindow).empty());
    assert(Bluefruit.Advertising.stop_calls == stops_done);
    assert(Bluefruit.Advertising.start_calls == starts_done);
    assert(bleQuery() ==
           "BLE ready=yes advertising=no connected=0 policy=closed initial_start=ok\n");
    assert(critical_depth == 0);
    // The Periph/ada_callback path was never used by production.
    assert(Bluefruit.Periph.disconnect_cb == nullptr &&
           Bluefruit.Periph.pending_disconnect_cbs == 0);
  }

  assert(munmap(region, kRegionSize) == 0);
  assert(munmap(config_region, kConfigRegionSize) == 0);
  printf("Production startup identity/history/loop (%s): PASS\n", argv[1]);
}
