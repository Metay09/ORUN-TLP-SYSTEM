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
  const bool success = mode == "success";
  // Each scenario runs in a new process, like a cold boot (static driver gate).
  assert(success || mode == "mutex" || mode == "gate" || mode == "queue" ||
         mode == "lora");
  void* region = mmap(reinterpret_cast<void*>(kBaseAddress), kRegionSize,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(region == reinterpret_cast<void*>(kBaseAddress));
  memset(region, 0xFF, kRegionSize);

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
  setup();
  assert(board_reads == 1 && radio_manager.deviceId() == kHardwareId);
  assert(watchdog_starts == 1);
  assert(history.ready() && history.count() == 1);
  assert(history.diagnostics().recovery_corruptions == 0);
  assert(erases == 0 && programs == 0);
  assert(memcmp(region, before.data(), before.size()) == 0);
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

  assert(munmap(region, kRegionSize) == 0);
  printf("Production startup identity/history/loop (%s): PASS\n", argv[1]);
}
