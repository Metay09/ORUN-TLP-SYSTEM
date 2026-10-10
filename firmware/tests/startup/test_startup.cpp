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

bool fake_softdevice_enabled = false;
uint32_t fake_soc_event = 0;
uint32_t sd_softdevice_is_enabled(uint8_t* enabled) {
  *enabled = fake_softdevice_enabled ? 1 : 0;
  return NRF_SUCCESS;
}
uint32_t sd_flash_write(uint32_t* dst, const uint32_t* src, uint32_t count) {
  ++programs;
  const auto address = reinterpret_cast<uintptr_t>(dst);
  const auto end = address + count * 4;
  const bool in_history =
      address >= kBaseAddress && end <= kBaseAddress + kRegionSize;
  const bool in_config =
      address >= kFutureConfigRegionStart && end <= kFutureConfigRegionEnd;
  const bool in_security =
      address >= kFutureSecurityRegionStart && end <= kFutureSecurityRegionEnd;
  const bool in_geofence =
      address >= kGeofenceRegionStart && end <= kGeofenceRegionEnd;
  assert(in_history || in_config || in_security || in_geofence);
  for (uint32_t i = 0; i < count; ++i) {
    assert(dst[i] == UINT32_MAX);
    dst[i] &= src[i];
  }
  return NRF_SUCCESS;
}
uint32_t sd_flash_page_erase(uint32_t page) {
  ++erases;
  const uint32_t address = page * kPageSize;
  const bool in_history =
      address >= kBaseAddress && address < kBaseAddress + kRegionSize;
  const bool in_config =
      address >= kFutureConfigRegionStart && address < kFutureConfigRegionEnd;
  const bool in_security =
      address >= kFutureSecurityRegionStart && address < kFutureSecurityRegionEnd;
  const bool in_geofence =
      address >= kGeofenceRegionStart && address < kGeofenceRegionEnd;
  assert(in_history || in_config || in_security || in_geofence);
  memset(reinterpret_cast<void*>(uintptr_t(page) * kPageSize), 0xFF, kPageSize);
  return NRF_SUCCESS;
}
// Most startup scenarios keep SoftDevice disabled, matching the original
// synchronous harness. The history_erase_i2c scenario enables it only after
// setup and explicitly controls completion delivery so a real production loop
// sees a multi-pass asynchronous erase.
uint32_t sd_evt_get(uint32_t* event) {
  if (fake_soc_event == 0) return NRF_ERROR_NOT_FOUND;
  *event = fake_soc_event;
  fake_soc_event = 0;
  return NRF_SUCCESS;
}

bool orun_tlp::NrfHistoryIncarnationSource::generate(uint64_t& incarnation) {
  // Host startup composition stub: this harness tests startup ownership and
  // History recovery, not the nRF52840 hardware RNG implementation.
  incarnation = 0xA1A2A3A4A5A6A7A8ULL;
  return true;
}

bool orun_tlp::NrfConfigIncarnationSource::generate(uint64_t& incarnation) {
  // Host startup composition stub: production implementation is target-only
  // hardware RNG and is independently compiler/link checked by the RAK build.
  incarnation = 0x0102030405060708ULL;
  return true;
}

bool orun_tlp::NrfGeofenceIncarnationSource::generate(uint64_t& incarnation) {
  // Same composition-only stub as Config: the real nRF RNG implementation is
  // independently compiled/linked by the RAK4630 production build.
  incarnation = 0x1020304050607080ULL;
  return true;
}

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
uint64_t orun_tlp::monotonic::nowMs64() { return test_now; }
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
  const bool geofence_scenario = mode == "geofence";
  const bool persisted_geofence_scenario = mode == "geofence_persisted";
  const bool uncertain_geofence_scenario = mode == "geofence_uncertain";
  const bool history_erase_i2c_scenario = mode == "history_erase_i2c";
  const bool success = mode == "success" || ble_advertising_fails || ble_runtime_fails ||
                       no_event_control || geofence_scenario ||
                       persisted_geofence_scenario || uncertain_geofence_scenario ||
                       history_erase_i2c_scenario;
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
  // M6D3C: production now recovers GeofenceStore before BLE, so map its
  // dedicated M6D3B partition for the real startup composition as well.
  constexpr uint32_t kGeofenceRegionSize =
      kGeofenceRegionEnd - kGeofenceRegionStart;
  void* geofence_region = mmap(
      reinterpret_cast<void*>(kGeofenceRegionStart), kGeofenceRegionSize,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
      -1, 0);
  assert(geofence_region == reinterpret_cast<void*>(kGeofenceRegionStart));
  memset(geofence_region, 0xFF, kGeofenceRegionSize);

  // M6D3C startup scenarios:
  // - geofence_persisted: one real committed CONFIGURED record with VALID token;
  // - geofence_uncertain: the same authoritative CONFIGURED record plus a
  //   committed-corrupt peer page. M6D3B intentionally preserves the readable
  //   semantic snapshot but marks mutation/CAS authority UNCERTAIN.
  // All other scenarios leave the partition blank so setup() must establish
  // the authoritative CLEAR baseline without configuring M6D2.
  if (persisted_geofence_scenario || uncertain_geofence_scenario) {
    const GeoPointE7 persisted_vertices[] = {
        GeoPointE7(409999000, 289999000),
        GeoPointE7(409999000, 290001000),
        GeoPointE7(410001000, 290001000),
        GeoPointE7(410001000, 289999000)};
    const GeofencePolygonView persisted_polygon(persisted_vertices, 4);
    geofence_format::Snapshot snapshot;
    assert(geofence_format::canonicalizeConfiguredAreaSet(
        GeofenceAreaSetView(&persisted_polygon, 1), snapshot));

    geofence_format::Record record;
    record.generation = 7;
    record.token = geofence_format::StateToken(0xA1A2A3A4A5A6A7A8ULL, 7);
    record.snapshot = snapshot;
    uint8_t encoded[geofence_format::kRecordSize]{};
    assert(geofence_format::encode(record, encoded, sizeof(encoded)));
    memcpy(geofence_region, encoded, sizeof(encoded));

    if (uncertain_geofence_scenario) {
      // Page B looks committed (commit word remains 0) but its body no longer
      // passes CRC. This is exactly the recovery class where M6D3B preserves
      // page A's semantic CONFIGURED snapshot read-only and sets token UNCERTAIN.
      geofence_format::Record corrupt_peer = record;
      corrupt_peer.generation = 8;
      corrupt_peer.token.revision = 8;
      uint8_t corrupt_encoded[geofence_format::kRecordSize]{};
      assert(geofence_format::encode(
          corrupt_peer, corrupt_encoded, sizeof(corrupt_encoded)));
      corrupt_encoded[geofence_format::kCrcOffset] ^= 0x01U;
      memcpy(static_cast<uint8_t*>(geofence_region) + kPageSize,
             corrupt_encoded, sizeof(corrupt_encoded));
    }
  }

  // Seed page 0 through the real journal/backend, including a committed fix.
  NrfHistoryFlash seed_flash;
  NrfHistoryIncarnationSource seed_incarnation_source;
  HistoryStore seed(seed_flash, &seed_incarnation_source);
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

  // M6D2 legacy startup composition scenario: install an explicit host-only
  // area set before the first accepted fix. M6D3C's separate
  // geofence_persisted scenario exercises the real durable production source.
  static const GeoPointE7 geofence_vertices[] = {
      GeoPointE7(409999000, 289999000),
      GeoPointE7(409999000, 290001000),
      GeoPointE7(410001000, 290001000),
      GeoPointE7(410001000, 289999000)};
  static const GeofencePolygonView geofence_areas[] = {
      GeofencePolygonView(geofence_vertices, 4)};
  if (geofence_scenario) {
    assert(geofence_confirmation.configure(
               GeofenceAreaSetView(geofence_areas, 1)) ==
           GeofenceRuntimeConfigResult::kApplied);
  }

  assert(board_reads == 1 && radio_manager.deviceId() == kHardwareId);
  assert(watchdog_starts == 1);
  // M7P7I is RAM-only and never reconstructs a product Location from recovered
  // HistoryStore contents at boot.
  assert(!location_owner.hasLocation());
  assert(history.ready() && history.count() == 1);
  assert(history.diagnostics().recovery_corruptions == 0);
  // Fresh blank ConfigStore establishes its internal v2 token baseline.
  // Blank GeofenceStore also establishes its own CLEAR baseline; persisted and
  // UNCERTAIN-fallback scenarios recover pre-seeded evidence without a write.
  const unsigned expected_boot_programs =
      (persisted_geofence_scenario || uncertain_geofence_scenario) ? 2U : 4U;
  assert(erases == 0 && programs == expected_boot_programs);
  assert(config_store.tokenState() == ConfigTokenState::kValid);
  assert(!config_store.hasCommittedRecord());  // application still "default"
  assert(geofence_store.ready());
  if (persisted_geofence_scenario) {
    assert(geofence_store.resourceState() == GeofenceResourceState::kConfigured);
    assert(geofence_store.tokenState() == GeofenceTokenState::kValid);
    assert(geofence_confirmation.configured());
    assert(Serial.output.find(
               "GEOFENCE runtime configured areas=1 vertices=4 token=VALID\n") !=
           std::string::npos);
  } else if (uncertain_geofence_scenario) {
    assert(geofence_store.resourceState() == GeofenceResourceState::kConfigured);
    assert(geofence_store.tokenState() == GeofenceTokenState::kUncertain);
    assert(geofence_confirmation.configured());
    assert(Serial.output.find(
               "GEOFENCE runtime configured areas=1 vertices=4 token=UNCERTAIN\n") !=
           std::string::npos);
    // Recovery is read-only: neither baseline creation nor geofence mutation
    // is allowed merely to "repair" uncertain evidence during production boot.
    assert(programs == 2U);
  } else {
    assert(geofence_store.resourceState() == GeofenceResourceState::kClear);
    assert(geofence_store.tokenState() == GeofenceTokenState::kValid);
    if (!geofence_scenario) assert(!geofence_confirmation.configured());
    assert(Serial.output.find("GEOFENCE runtime clear token=VALID\n") !=
           std::string::npos);
  }
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

  if (history_erase_i2c_scenario) {
    // M2 audit regression: exercise the actual production loop, not only
    // HistoryStore::erasePending() in isolation. First drive the real GNSS
    // state machine into kAcquiring so checkUblox() is an observable loop-owned
    // client during the erase window. Then arrange one almost-full active page
    // synchronously and enable the SoftDevice path so an accepted erase remains
    // pending across multiple loop passes.
    for (unsigned i = 0; i < 16; ++i) {
      test_now += i < 2 ? gnss_config::kPowerSettleMs : 10;
      loop();
    }
    assert(gnss_manager.state() == GnssManager::State::kAcquiring);
    const unsigned active_gnss_reads = SFE_UBLOX_GNSS::reads;
    loop();
    assert(SFE_UBLOX_GNSS::reads > active_gnss_reads);

    auto allocateFixturePacket = [&](int32_t latitude_e7, uint8_t* bytes,
                                     uint64_t& identity) {
      if (!history.canAppend()) {
        assert(!history.prepareAppend());
        settle(history);
      }
      uint32_t fixture_sequence = 0;
      assert(history.nextSequence(fixture_sequence, identity));
      const tlp::PositionPacket fixture{
          kHardwareId, fixture_sequence, 0, latitude_e7, 290000000,
          10, 100, 8, tlp::kPositionFlagValidFix};
      assert(tlp::serializePositionPacket(
          fixture, bytes, tlp::kPositionPacketSize));
    };

    auto appendFixtureSync = [&](int32_t latitude_e7) {
      uint8_t bytes[tlp::kPositionPacketSize]{};
      uint64_t identity = 0;
      allocateFixturePacket(latitude_e7, bytes, identity);
      assert(history.append(bytes, identity));
      settle(history);
      bool stored = false;
      assert(history.takeAppendResult(stored) && stored);
    };

    while (history.count() < 102U)
      appendFixtureSync(410000100 + static_cast<int32_t>(history.count()));
    assert(history.count() == 102U);

    // A normal record program may be asynchronously pending, but it must NOT
    // quiesce the loop-owned Wire clients. Reset the accelerometer probe before
    // each pass so a missing main-loop poll is observable as zero transactions.
    fake_softdevice_enabled = true;
    uint8_t normal_packet[tlp::kPositionPacketSize]{};
    uint64_t normal_identity = 0;
    allocateFixturePacket(410000300, normal_packet, normal_identity);
    assert(history.append(normal_packet, normal_identity));
    assert(history.busy() && !history.erasePending());

    accelerometer_manager.begin(test_now);
    unsigned wire_before = Wire.transaction_calls;
    unsigned gnss_reads_before = SFE_UBLOX_GNSS::reads;
    loop();  // submits record body -> async pending
    assert(Wire.transaction_calls > wire_before);
    assert(SFE_UBLOX_GNSS::reads > gnss_reads_before);
    assert(history.busy() && !history.erasePending());

    accelerometer_manager.begin(test_now);
    wire_before = Wire.transaction_calls;
    gnss_reads_before = SFE_UBLOX_GNSS::reads;
    loop();  // body is still pending: both loop-owned I2C clients must run
    assert(Wire.transaction_calls > wire_before);
    assert(SFE_UBLOX_GNSS::reads > gnss_reads_before);
    assert(history.busy() && !history.erasePending());

    fake_soc_event = NRF_EVT_FLASH_OPERATION_SUCCESS;
    accelerometer_manager.begin(test_now);
    loop();  // body completes; commit is submitted
    assert(history.busy() && !history.erasePending());
    fake_soc_event = NRF_EVT_FLASH_OPERATION_SUCCESS;
    accelerometer_manager.begin(test_now);
    loop();  // commit completes
    bool normal_stored = false;
    assert(history.takeAppendResult(normal_stored) && normal_stored);
    assert(history.count() == 103U);

    // Fill the final slot synchronously so the next append enters kNewPage /
    // kErase before the production loop executes.
    fake_softdevice_enabled = false;
    appendFixtureSync(410000301);
    assert(history.count() == 104U);

    fake_softdevice_enabled = true;
    uint8_t rotating_packet[tlp::kPositionPacketSize]{};
    uint64_t rotating_identity = 0;
    allocateFixturePacket(410000302, rotating_packet, rotating_identity);
    assert(history.append(rotating_packet, rotating_identity));
    assert(history.erasePending());

    accelerometer_manager.begin(test_now);
    wire_before = Wire.transaction_calls;
    gnss_reads_before = SFE_UBLOX_GNSS::reads;
    loop();  // submits erase -> kPending; both loop-owned I2C clients skipped
    assert(Wire.transaction_calls == wire_before);
    assert(SFE_UBLOX_GNSS::reads == gnss_reads_before);
    assert(history.erasePending());

    accelerometer_manager.begin(test_now);
    wire_before = Wire.transaction_calls;
    gnss_reads_before = SFE_UBLOX_GNSS::reads;
    loop();  // erase still pending without completion
    assert(Wire.transaction_calls == wire_before);
    assert(SFE_UBLOX_GNSS::reads == gnss_reads_before);
    assert(history.erasePending());

    fake_soc_event = NRF_EVT_FLASH_OPERATION_SUCCESS;
    accelerometer_manager.begin(test_now);
    wire_before = Wire.transaction_calls;
    gnss_reads_before = SFE_UBLOX_GNSS::reads;
    loop();  // completion is consumed, but this pass began inside erase window
    assert(Wire.transaction_calls == wire_before);
    assert(SFE_UBLOX_GNSS::reads == gnss_reads_before);
    assert(!history.erasePending());

    // The next pass must immediately resume both loop-owned I2C clients even
    // though History is still busy with the new-page header program. This
    // distinguishes erase-only quiescence from an accidental "all flash
    // mutation" gate.
    accelerometer_manager.begin(test_now);
    wire_before = Wire.transaction_calls;
    gnss_reads_before = SFE_UBLOX_GNSS::reads;
    loop();
    assert(Wire.transaction_calls > wire_before);
    assert(SFE_UBLOX_GNSS::reads > gnss_reads_before);
    assert(history.busy() && !history.erasePending());

  assert(munmap(region, kRegionSize) == 0);
    assert(munmap(config_region, kConfigRegionSize) == 0);
    assert(munmap(security_region, kSecurityRegionSize) == 0);
    assert(munmap(geofence_region, kGeofenceRegionSize) == 0);
    printf("Production startup identity/history/loop (history_erase_i2c): PASS\n");
    return 0;
  }

  // Complete recovery and GNSS detection/configuration through the actual loop.
  for (unsigned i = 0; i < 16; ++i) {
    // Wait for both rail transitions, then service the loop at normal cadence.
    test_now += i < 2 ? gnss_config::kPowerSettleMs : 10;
    loop();
  }
  assert(watchdog_feeds == 16 && fake_idle_calls == 16);
  assert(gnss_manager.state() == GnssManager::State::kAcquiring);
  assert(role_controller.role() == NodeRole::kTracker);
  // Recovery plus ordinary runtime polling must remain read-only for History.
  // No sequence reservation is allowed until a real fresh fix is pending.
  assert(erases == 0 && programs == expected_boot_programs);
  assert(memcmp(region, before.data(), before.size()) == 0);
  HistoryStore::Record recovered{};
  assert(history.lookup(original.identity, recovered));
  assert(memcmp(recovered.packet, original.packet, sizeof(original.packet)) == 0);

  // Feed a fresh matched PVT/DOP through production GNSS -> store-first flow.
  // Reuse the existing noevent startup scenario as the 2D Location-validity
  // regression; all other scenarios exercise the ordinary 3D path.
  using Fake = SFE_UBLOX_GNSS;
  const uint8_t initial_fix_type = no_event_control ? 2U : 3U;
  for (uint32_t tow : {1000U, 2000U}) {
    Fake::pending.push_back([tow, initial_fix_type] {
      UBX_NAV_PVT_data_t pvt{};
      pvt.iTOW = tow; pvt.flags.bits.gnssFixOK = true;
      pvt.fixType = initial_fix_type;
      pvt.lat = 410000001; pvt.lon = 290000001; pvt.numSV = 8;
      Fake::current_pvt = pvt; Fake::itow_fresh = true;
      Fake::pvt(&pvt);
      UBX_NAV_DOP_data_t dop{tow, 100};
      Fake::dop(&dop);
    });
    loop(); // First epoch establishes the R3 boundary; second is fresh.
  }
  // The fresh fix first creates real demand for a sequence reservation.
  // It is not consumed yet, so Location must still be UNKNOWN at this exact
  // pre-admission point. The next loop completes the reservation and reaches
  // the existing successful takeFreshFixForTransmission() acceptance point.
  assert(!location_owner.hasLocation());
  loop();
  // M7P7I publishes from that exact accepted observation before the record
  // itself is committed on the following loop. Recovered history was never
  // used to manufacture a Location.
  AcceptedLocation accepted_location{};
  assert(location_owner.latest(&accepted_location));
  assert(accepted_location.source == LocationSource::kGnss);
  assert(accepted_location.latitude_e7 == 410000001);
  assert(accepted_location.longitude_e7 == 290000001);
  assert(accepted_location.altitude_valid == !no_event_control);
  assert(!accepted_location.utc_valid);
  assert(accepted_location.observed_monotonic_ms == test_now);
  // The record itself is not committed until the following loop.
  assert(history.count() == 1 && erases == 0 &&
         programs == expected_boot_programs + 2U);
  assert(history.busy());
  loop();
  assert(history.count() == 2 && erases == 0 &&
         programs == expected_boot_programs + 4U);
  assert(history.newest(recovered) && recovered.identity > original.identity);
  tlp::PositionPacket newest{};
  assert(tlp::deserializePositionPacket(recovered.packet, sizeof(recovered.packet), &newest));
  assert(newest.source_device_id == kHardwareId && newest.latitude_e7 == 410000001);
  assert(send_calls == (success ? 1U : 0U));
  assert(radio_manager.isTransmitting() == success);
  if (geofence_scenario) {
    assert(geofence_confirmation.cadenceMode() == GeofenceCadenceMode::kBase);
    assert(!geofence_confirmation.confirmationActive());
    assert(gnss_manager.trackingIntervalMs() == 180000);

    auto finishTx = [&]() {
      if (!radio_manager.isTransmitting()) return;
      {
        radio_driver::Guard gate;
        assert(gate);
        callbacks->TxDone();
      }
      // Owner loop must run only after the callback-side gate is released.
      loop();
      assert(!radio_manager.isTransmitting());
    };

    auto queueEpoch = [&](uint32_t tow, int32_t lat, int32_t lon,
                          uint16_t hdop, uint8_t satellites) {
      Fake::pending.push_back([=] {
        UBX_NAV_PVT_data_t pvt{};
        pvt.iTOW = tow;
        pvt.flags.bits.gnssFixOK = true;
        pvt.fixType = 3;
        pvt.lat = lat;
        pvt.lon = lon;
        pvt.numSV = satellites;
        Fake::current_pvt = pvt;
        Fake::itow_fresh = true;
        Fake::pvt(&pvt);
        UBX_NAV_DOP_data_t dop{tow, hdop};
        Fake::dop(&dop);
      });
    };

    auto startNextAcquisition = [&]() {
      const uint32_t attempts_before =
          gnss_manager.diagnostics().acquisition_attempts;
      test_now += gnss_manager.trackingIntervalMs();
      loop();
      for (unsigned i = 0;
           i < 16 && gnss_manager.state() != GnssManager::State::kAcquiring;
           ++i) {
        test_now += i == 0 ? gnss_config::kPowerSettleMs : 10;
        loop();
      }
      assert(gnss_manager.state() == GnssManager::State::kAcquiring);
      assert(gnss_manager.diagnostics().acquisition_attempts ==
             attempts_before + 1);
      return gnss_manager.diagnostics().acquisition_attempts;
    };

    auto drainPosition = [&]() {
      for (unsigned i = 0; i < 24; ++i) {
        if (radio_manager.isTransmitting()) finishTx();
        if (!positions.pending() && !radio_manager.isTransmitting()) return;
        loop();
      }
      assert(!positions.pending() && !radio_manager.isTransmitting());
    };

    // Finish the normal INSIDE report before starting transition exercises.
    finishTx();
    drainPosition();

    // Exact main-loop timeout path: the candidate is ordinary POSITION slot 0,
    // continuation starts in the same acquisition, then the 10 s deadline
    // aborts before any extra evidence is allowed to decide OUTSIDE.
    startNextAcquisition();
    queueEpoch(3000, 410010000, 290010000, 100, 8);  // session boundary
    loop();
    const uint32_t timeout_records_before = history.count();
    queueEpoch(4000, 410010000, 290010000, 60, 10);  // OUTSIDE candidate
    loop();
    assert(geofence_confirmation.confirmationActive());
    assert(gnss_manager.additionalFixAcquisitionActive());
    const uint32_t attempts_during_timeout =
        gnss_manager.diagnostics().acquisition_attempts;
    test_now += geofence_confirmation_config::kConfirmationDeadlineMs;
    loop();
    assert(!geofence_confirmation.confirmationActive());
    assert(!gnss_manager.additionalFixAcquisitionActive());
    assert(geofence_confirmation.cadenceMode() == GeofenceCadenceMode::kBase);
    assert(gnss_manager.trackingIntervalMs() == 180000);
    assert(gnss_manager.diagnostics().acquisition_attempts ==
           attempts_during_timeout);
    assert(Serial.output.find("GEOFENCE confirmation timeout") !=
           std::string::npos);
    drainPosition();
    assert(history.count() == timeout_records_before + 1);

    // Successful transition. Slot 0 has the best OUTSIDE HDOP and was already
    // accepted by the ordinary store-first path. Selecting it as the logical
    // representative must NOT create a duplicate record/sequence/TX.
    startNextAcquisition();
    queueEpoch(5000, 410010000, 290010000, 100, 8);  // session boundary
    loop();
    const uint32_t records_before_transition = history.count();
    const unsigned sends_before_transition = send_calls;
    queueEpoch(6000, 410010000, 290010000, 50, 10);  // slot 0, best OUTSIDE
    loop();
    assert(geofence_confirmation.confirmationActive());
    assert(geofence_slot0_normal_store_accepted);
    const uint32_t confirmation_attempts =
        gnss_manager.diagnostics().acquisition_attempts;

    queueEpoch(7000, 410011000, 290011000, 90, 9);   // slot 1 OUTSIDE
    loop();
    assert(geofence_confirmation.confirmationActive());
    assert(gnss_manager.diagnostics().acquisition_attempts ==
           confirmation_attempts);
    // Confirmation-only observations are accepted Location facts even though
    // they intentionally bypass the normal PositionFlow branch.
    assert(location_owner.latest(&accepted_location));
    assert(accepted_location.latitude_e7 == 410011000);
    assert(accepted_location.longitude_e7 == 290011000);

    queueEpoch(8000, 410000001, 290000001, 20, 12); // slot 2 INSIDE
    loop();
    assert(location_owner.latest(&accepted_location));
    assert(accepted_location.latitude_e7 == 410000001);
    assert(accepted_location.longitude_e7 == 290000001);
    assert(!geofence_confirmation.confirmationActive());
    assert(geofence_confirmation.cadenceMode() ==
           GeofenceCadenceMode::kBaseDividedBy3);
    assert(gnss_manager.trackingIntervalMs() == 60000);
    assert(gnss_manager.diagnostics().acquisition_attempts ==
           confirmation_attempts);
    assert(!geofence_representative_pending);
    assert(Serial.output.find(
               "GEOFENCE OUTSIDE confirmed (local event occurrence)") !=
           std::string::npos);

    drainPosition();
    assert(history.count() == records_before_transition + 1);
    assert(send_calls == sends_before_transition + 1);

    // Re-anchor is from confirmation completion. 60 s uses the existing
    // keep-powered policy; no catch-up or acquisition may start at +59.999 s.
    loop();
    assert(gnss_manager.state() == GnssManager::State::kIdle);
    const uint32_t attempts_after_confirmation =
        gnss_manager.diagnostics().acquisition_attempts;
    test_now += 59999;
    loop();
    assert(gnss_manager.diagnostics().acquisition_attempts ==
           attempts_after_confirmation);
    test_now += 1;
    loop();
    assert(gnss_manager.diagnostics().acquisition_attempts ==
           attempts_after_confirmation + 1);

    assert(munmap(region, kRegionSize) == 0);
    assert(munmap(config_region, kConfigRegionSize) == 0);
    printf("Production startup identity/history/loop (geofence): PASS\n");
    return 0;
  }
  if (!success) {
    assert(rx_calls == 0);
    assert(!radio_manager.sendPositionPacket(recovered.packet));
    assert(send_calls == 0);
  }
  // 16 startup service loops + 2 GNSS epochs + 2 lazy History steps
  // (reservation commit, then record commit). Every production loop must still
  // feed the watchdog and enter the idle hook exactly once.
  assert(watchdog_feeds == 20 && fake_idle_calls == 20);
  // Each of those passes is also visible to the independent liveness timer,
  // and a finished pass leaves the loop marked as idle, so a stall record can
  // tell "stuck inside a step" from "never woke up".
  assert(LoopHealthMonitor::completedPasses() == 20);
  assert(LoopHealthMonitor::currentStageCode() ==
         static_cast<uint8_t>(LoopStage::kIdle));
  Serial.output.clear();
  Serial.queueInput("HEALTH?\n");
  pollRoleCommands();
  assert(Serial.output ==
         "HEALTH reset=0x00000000 watchdog=no prev_stall=no prev_fault=no "
         "prev_stage=- idle_oversleeps=0 idle_longest_ms=0 "
         "stalls_recovered=0 stack_free=0\n");

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

    // M7P7H production composition: an additive status request travels through
    // the same callback -> loop -> ApplicationRequestService -> indication path.
    // GEOFENCE is intentionally a one-fragment summary: no geometry/token bytes.
    const uint8_t get_geofence_1[] = {
        0x01, 0x04, 0x03, 0x00, 0x11, 0x00, 0x00, 0x00};
    const unsigned status_hvx_before = BluefruitHvx.calls;
    ble_application_request_characteristic.simulateWrite(
        Bluefruit.connHandle(), get_geofence_1, sizeof(get_geofence_1));
    assert(tick(0).empty());
    assert(BluefruitHvx.calls == status_hvx_before + 1);
    assert(BluefruitHvx.len == 17);  // 8-byte frame header + 9-byte summary.
    assert(BluefruitHvx.data[0] == 0x01 && BluefruitHvx.data[1] == 0x84);
    assert(BluefruitHvx.data[4] == 0x11 && BluefruitHvx.data[5] == 0x00);
    assert(BluefruitHvx.data[6] == 0x09 && BluefruitHvx.data[7] == 0x00);
    assert(ble_application_indication_in_flight);
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

  // M7P7H value-level production composition guard. Exercise the real
  // owner->snapshot mapper used by USB/BLE rather than only synthetic PODs.
  refreshApplicationStatusSnapshot(test_now);
  assert(application_status_snapshot.populated == 1);
  assert(application_status_snapshot.device.uptime_ms_mod32 == test_now);
  assert(strcmp(application_status_snapshot.device.firmware_version,
                kFirmwareVersion) == 0);
  assert(application_status_snapshot.tracking.requested_interval_seconds ==
         config_store.config().tracking_interval_seconds);
  assert(application_status_snapshot.tracking.applied_base_interval_seconds ==
         active_tracking_base_interval_seconds);
  assert(application_status_snapshot.tracking.effective_interval_seconds ==
         gnss_manager.trackingIntervalMs() / 1000UL);
  assert(application_status_snapshot.geofence.area_count ==
         geofence_store.areaCount());
  assert(application_status_snapshot.geofence.total_vertex_count ==
         geofence_store.totalVertexCount());
  assert(application_status_snapshot.storage.history_count == history.count());
  assert(application_status_snapshot.storage.history_capacity ==
         history.capacity());
  assert(application_status_snapshot.storage.config_ready ==
         (config_store.ready() ? 1U : 0U));
  assert(application_status_snapshot.storage.config_maintenance ==
         (config_store.maintenanceResetRequired() ? 1U : 0U));
  assert(application_status_snapshot.storage.geofence_ready ==
         (geofence_store.ready() ? 1U : 0U));
  assert(application_status_snapshot.storage.geofence_maintenance ==
         (geofence_store.maintenanceResetRequired() ? 1U : 0U));
  assert(application_status_snapshot.storage.security_ready ==
         (security_store.ready() ? 1U : 0U));
  assert(application_status_snapshot.storage.security_exhausted ==
         (security_store.exhausted() ? 1U : 0U));
  assert(application_status_snapshot.tracking.config_backend_ready ==
         (config_store.ready() ? 1U : 0U));
  assert(application_status_snapshot.tracking.config_has_committed_record ==
         (config_store.hasCommittedRecord() ? 1U : 0U));
  const auto& hdiag = history.diagnostics();
  assert(application_status_snapshot.storage.history_overwritten ==
         hdiag.overwritten);
  assert(application_status_snapshot.storage.history_append_failures ==
         hdiag.append_failures);
  assert(application_status_snapshot.storage.history_recovery_corruptions ==
         hdiag.recovery_corruptions);
  assert(application_status_snapshot.storage.history_metadata_failures ==
         hdiag.metadata_failures);
  {
    GeofenceOperationalState owner_state{};
    const bool owner_has_state =
        geofence_confirmation.getConfirmedState(&owner_state);
    assert(application_status_snapshot.geofence.has_confirmed_state ==
           (owner_has_state ? 1U : 0U));
    if (owner_has_state) {
      const ApplicationGeofenceOperationalState expected =
          owner_state == GeofenceOperationalState::kOutside
              ? ApplicationGeofenceOperationalState::kOutside
              : ApplicationGeofenceOperationalState::kInside;
      assert(application_status_snapshot.geofence.confirmed_state == expected);
    } else {
      assert(application_status_snapshot.geofence.confirmed_state ==
             ApplicationGeofenceOperationalState::kUnknown);
    }
  }

  // Configuration write path through the REAL production loop:
  // USB text -> ConfigMutationOwner -> ConfigStore flash save -> runtime
  // apply -> typed result. Removing config_mutations.poll(),
  // applyCommittedConfigToRuntime() or drainConfigMutationResult() from
  // loop() must fail here rather than pass through a manual call.
  if (mode == "success") {
    assert(config_store.ready() && !config_store.maintenanceResetRequired());
    assert(!config_store.hasCommittedRecord());
    assert(active_tracking_base_interval_seconds == 180);
    assert(gnss_manager.trackingIntervalMs() == 180000UL);
    const unsigned write_erases = erases;
    const unsigned write_programs = programs;
    next_usb_application_request_id = 40;

    Serial.output.clear();
    Serial.queueInput("APP INTERVAL 600\n");
    for (int pass = 0; pass < 16 && !has(Serial.output, "APP SET id=40");
         ++pass) {
      loop();
    }
    assert(has(Serial.output,
               "CONFIG applied base_s=600 effective_ms=600000\n"));
    assert(has(Serial.output,
               "APP SET id=40 code=APPLIED tracking_interval_seconds=600 "
               "token=VALID revision=2\n"));
    // The runtime adopts the change before the result is reported.
    assert(Serial.output.find("CONFIG applied") <
           Serial.output.find("APP SET id=40"));
    assert(erases == write_erases + 1 && programs > write_programs);
    assert(config_store.config().tracking_interval_seconds == 600);
    assert(config_store.hasCommittedRecord());
    assert(active_tracking_base_interval_seconds == 600);
    assert(gnss_manager.trackingIntervalMs() == 600000UL);
    assert(!config_mutations.busy());

    // The same value again writes nothing and does not move the token.
    const unsigned settled_erases = erases;
    const unsigned settled_programs = programs;
    Serial.output.clear();
    Serial.queueInput("APP INTERVAL 600\n");
    loop();
    assert(has(Serial.output,
               "APP SET id=41 code=UNCHANGED tracking_interval_seconds=600 "
               "token=VALID revision=2\n"));
    assert(!has(Serial.output, "CONFIG applied"));

    // Below the writer floor: refused, stored value reported back.
    Serial.output.clear();
    Serial.queueInput("APP INTERVAL 59\n");
    loop();
    assert(has(Serial.output,
               "APP SET id=42 code=INVALID tracking_interval_seconds=600 "
               "token=VALID revision=2\n"));

    // Unusable arguments never reach the owner and take no request id.
    Serial.output.clear();
    Serial.queueInput("APP INTERVAL 6x\nAPP INTERVAL \n");
    loop();
    assert(has(Serial.output, "APP REJECTED\nAPP REJECTED\n"));
    assert(!has(Serial.output, "APP SET"));
    assert(next_usb_application_request_id == 43);
    assert(erases == settled_erases && programs == settled_programs);
    assert(active_tracking_base_interval_seconds == 600);

    // The read path now reports the stored value.
    Serial.output.clear();
    Serial.queueInput("APP CONFIG?\n");
    loop();
    assert(has(Serial.output,
               "source=stored tracking_interval_seconds=600 "
               "battery_capacity_mah=0\n"));
  }

  // M1 regression: resolved relay intent and actual radio application are
  // distinct. Do not run loop() between override and snapshot; the effective
  // config is RELAY while RadioManager still truthfully reports not applied.
  if (mode == "success") {
    assert(!radio_manager.relayForwardingEnabled());
    assert(role_controller.applyOverride(NodeRole::kRelay));
    refreshApplicationStatusSnapshot(test_now);
    assert(application_status_snapshot.device.relay_state ==
           ApplicationServiceState::kEnabled);
    assert(application_status_snapshot.device.relay_reason ==
           ApplicationServiceReason::kNone);
    assert(application_status_snapshot.device.relay_forwarding_applied == 0);

    Serial.output.clear();
    startUsbApplicationQuery(ApplicationRequestKind::kGetDeviceStatus);
    drainApplicationResponse();
    assert(has(Serial.output, "relay_applied=no"));
  }

  assert(munmap(region, kRegionSize) == 0);
  assert(munmap(config_region, kConfigRegionSize) == 0);
  assert(munmap(security_region, kSecurityRegionSize) == 0);
  assert(munmap(geofence_region, kGeofenceRegionSize) == 0);
  printf("Production startup identity/history/loop (%s): PASS\n", argv[1]);
}
