// SF5E TEST-ONLY physical ObservationStore qualification on the product
// region [0x0C5000, 0x0E5000). Development RAK4631 only; never deploy.
//
// The real production pieces run on the real flash: ObservationStore ->
// FlashMutationGate::productPort() -> NrfProductFlash (SoftDevice disabled).
// A thin wrapper counts the store's flash mutations and, only after an
// explicit "SF5E CUT" command, interrupts one of them:
//   BEFORE  reset before the write/erase;
//   TORN    write the first half of a program (word aligned), then reset
//           (the state a power cut leaves mid-program); an erase acts as
//           BEFORE;
//   AFTER   complete the write/erase, then reset before the store sees it;
//   HOLD    stop before the write/erase and wait for a REAL power cut.
// Boot never mutates: it mounts only an erased region or the store's own
// pages (product_region.h). "SF5E PREPARE CONFIRM" is the only erase of the
// whole region. VERIFY walks the raw pages; PROTECT prints CRC32s of every
// other reserved region and the bootloader so the operator can prove they
// stayed untouched.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf.h>
#include <nrf_sdm.h>
#include <stdlib.h>
#include <string.h>

#include "flash_mutation_gate.h"
#include "observation_store.h"
#include "observation_store_format.h"
#include "product_region.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::storage_config;
namespace osf = orun_tlp::observation_store_format;
namespace pr = orun_tlp::product_region;

namespace {

constexpr uint32_t kDataPages = kProductRegionPages - osf::kControlPageCount;
constexpr uint32_t kMaxRecords = kDataPages * osf::kDataRecordsPerPage;
constexpr uint8_t kPayloadSize = osf::kPeriodicPayloadSizeV1;

void feedWatchdog() {
  if (NRF_WDT->RUNSTATUS != 0) NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}

uint8_t patternByte(uint32_t sequence, unsigned index) {
  return static_cast<uint8_t>(sequence * 131U + index * 7U + 0x5AU);
}

class RngIncarnation final : public ObservationIncarnationSource {
 public:
  bool generate(uint64_t& value) override {
    const uint32_t saved = NRF_RNG->CONFIG;
    NRF_RNG->CONFIG = 1U;  // bias correction
    NRF_RNG->EVENTS_VALRDY = 0;
    NRF_RNG->TASKS_START = 1;
    uint64_t candidate = 0;
    bool ok = true;
    for (unsigned i = 0; i < 8U && ok; ++i) {
      const uint32_t started = millis();
      while (NRF_RNG->EVENTS_VALRDY == 0) {
        if (millis() - started > 100U) {
          ok = false;
          break;
        }
      }
      NRF_RNG->EVENTS_VALRDY = 0;
      candidate = (candidate << 8) | (NRF_RNG->VALUE & 0xFFU);
    }
    NRF_RNG->TASKS_STOP = 1;
    NRF_RNG->CONFIG = saved;
    if (!ok || candidate == 0U) return false;
    value = candidate;
    return true;
  }
};

enum class CutMode : uint8_t { kNone, kBefore, kTorn, kAfter, kHold };

const char* cutModeName(CutMode mode) {
  switch (mode) {
    case CutMode::kNone: return "NONE";
    case CutMode::kBefore: return "BEFORE";
    case CutMode::kTorn: return "TORN";
    case CutMode::kAfter: return "AFTER";
    case CutMode::kHold: return "HOLD";
  }
  return "NONE";
}

class CutBackend final : public FlashBackend {
 public:
  explicit CutBackend(FlashBackend& inner) : inner_(inner) {}

  bool begin() override { return inner_.begin(); }
  bool read(uint32_t offset, void* data, size_t size) const override {
    return inner_.read(offset, data, size);
  }
  FlashOpResult pollPending() override { return inner_.pollPending(); }
  bool hasUnreconciledMutation() const override {
    return inner_.hasUnreconciledMutation();
  }

  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    ++calls_;
    if (armedHere()) {
      if (mode_ == CutMode::kTorn && size >= 8U) {
        const size_t half = (size / 2U) & ~size_t(3U);
        (void)inner_.program(offset, data, half);
        fire("PROGRAM", offset, size);
      }
      if (mode_ == CutMode::kAfter) {
        const FlashOpResult result = inner_.program(offset, data, size);
        fire(result == FlashOpResult::kDone ? "PROGRAM" : "PROGRAM_FAILED",
             offset, size);
      }
      fire("PROGRAM", offset, size);  // BEFORE, HOLD, or TORN of one word
    }
    return inner_.program(offset, data, size);
  }

  FlashOpResult erasePage(uint32_t page) override {
    ++calls_;
    if (armedHere()) {
      if (mode_ == CutMode::kAfter) {
        const FlashOpResult result = inner_.erasePage(page);
        fire(result == FlashOpResult::kDone ? "ERASE" : "ERASE_FAILED",
             page * kPageSize, kPageSize);
      }
      fire("ERASE", page * kPageSize, kPageSize);
    }
    return inner_.erasePage(page);
  }

  void arm(uint32_t call, CutMode mode) {
    target_ = calls_ + call;
    mode_ = mode;
  }
  void disarm() { mode_ = CutMode::kNone; }
  bool armed() const { return mode_ != CutMode::kNone; }
  uint32_t calls() const { return calls_; }

 private:
  bool armedHere() const {
    return mode_ != CutMode::kNone && calls_ == target_;
  }

  [[noreturn]] void fire(const char* operation, uint32_t offset, size_t size) {
    Serial.printf("SF5E CUT fired mode=%s op=%s offset=0x%05lX size=%u\n",
                  cutModeName(mode_), operation,
                  static_cast<unsigned long>(offset),
                  static_cast<unsigned>(size));
    Serial.flush();
    if (mode_ == CutMode::kHold) {
      while (true) {
        feedWatchdog();
        Serial.println(F("SF5E HOLD reached; CUT POWER NOW (USB and battery)"));
        Serial.flush();
        delay(1000);
      }
    }
    delay(100);
    NVIC_SystemReset();
    while (true) {
    }
  }

  FlashBackend& inner_;
  uint32_t calls_ = 0;
  uint32_t target_ = 0;
  CutMode mode_ = CutMode::kNone;
};

FlashMutationGate gate;
CutBackend cut_backend(gate.productPort());
RngIncarnation incarnation_source;
ObservationStore store(cut_backend, kProductRegionPages, &incarnation_source);
uint64_t device_id = 0;
bool mounted = false;
pr::MountDecision last_decision = pr::MountDecision::kNoOwner;

// Static workspaces keep large buffers off the loop task stack.
uint32_t verify_sequences[kMaxRecords];
uint8_t record_bytes[osf::kDataRecordSize];
uint8_t header_bytes[osf::kPageHeaderSize];
char command[48]{};
unsigned command_length = 0;

uint32_t crcRange(uint32_t start, uint32_t end) {
  const auto* bytes = reinterpret_cast<const volatile uint8_t*>(start);
  uint32_t crc = UINT32_MAX;
  for (uint32_t i = 0; i < end - start; ++i) {
    if ((i & 0x0FFFU) == 0U) feedWatchdog();
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8U; ++bit)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
  }
  return ~crc;
}

void printProtect() {
  // Everything reserved except the product region, plus the bootloader code
  // (its settings page changes on every firmware upload, so it is left out).
  Serial.printf(
      "SF5E PROTECT geofence=0x%08lX security=0x%08lX config=0x%08lX "
      "bond=0x%08lX history=0x%08lX bootloader=0x%08lX\n",
      static_cast<unsigned long>(crcRange(kGeofenceRegionStart, kGeofenceRegionEnd)),
      static_cast<unsigned long>(crcRange(kFutureSecurityRegionStart, kFutureSecurityRegionEnd)),
      static_cast<unsigned long>(crcRange(kFutureConfigRegionStart, kFutureConfigRegionEnd)),
      static_cast<unsigned long>(crcRange(kFutureBondRegionStart, kFutureBondRegionEnd)),
      static_cast<unsigned long>(crcRange(kBaseAddress, kBaseAddress + kRegionSize)),
      static_cast<unsigned long>(crcRange(0x0F4000U, 0x0FE000U)));
}

void settle() {
  for (uint32_t i = 0; i < 100000U && store.busy(); ++i) {
    store.poll();
    if ((i & 0x3FU) == 0U) feedWatchdog();
  }
}

void mount() {
  mounted = false;
  pr::Census census{};
  if (!gate.productPort().begin() ||
      !pr::takeCensus(gate.productPort(), kProductRegionPages, census)) {
    Serial.println(F("SF5E REGION read=FAIL"));
    return;
  }
  last_decision = pr::mountDecision(pr::Owner::kObservation, census);
  Serial.printf(
      "SF5E REGION erased=%u observation=%u custody=%u unknown=%u decision=%s\n",
      census.erased, census.observation, census.custody, census.unknown,
      pr::mountDecisionName(last_decision));
  if (last_decision != pr::MountDecision::kMount) return;
  mounted = store.begin(device_id);
  Serial.printf("SF5E MOUNT %s\n", mounted ? "PASS" : "FAIL");
}

void printStore() {
  osf::RecordIdentity next{};
  const bool have_next = mounted && store.peekNextIdentity(next);
  uint32_t retained = 0;
  uint32_t released = 0;
  const bool counted = mounted && store.retainedCount(retained) &&
                       store.releasedCount(released);
  const ObservationStore::Diagnostics& d = store.diagnostics();
  Serial.printf(
      "SF5E STORE mounted=%s ready=%s faulted=%s busy=%s next_seq=%lu "
      "retained=%ld released=%ld prepared=%s\n",
      mounted ? "yes" : "no", store.ready() ? "yes" : "no",
      store.faulted() ? "yes" : "no", store.busy() ? "yes" : "no",
      static_cast<unsigned long>(have_next ? next.sequence : 0U),
      counted ? static_cast<long>(retained) : -1L,
      counted ? static_cast<long>(released) : -1L,
      store.hasPreparedDataPage() ? "yes" : "no");
  // Adafruit's Print::printf formats into a 256-byte buffer: keep lines short.
  Serial.printf(
      "SF5E DIAG rec_ret=%lu rec_rel=%lu staged=%lu partial=%lu faults=%lu "
      "rot_rec=%lu reclaimed=%lu lost=%lu\n",
      static_cast<unsigned long>(d.recovered_retained),
      static_cast<unsigned long>(d.recovered_released),
      static_cast<unsigned long>(d.staged_records),
      static_cast<unsigned long>(d.partial_record_commits),
      static_cast<unsigned long>(d.recovery_faults),
      static_cast<unsigned long>(d.rotation_recoveries),
      static_cast<unsigned long>(d.pages_reclaimed),
      static_cast<unsigned long>(d.capacity_lost_total));
}

void printStatus() {
  printStore();
  printProtect();
  Serial.flush();
}

const char* maintenanceName(ObservationStore::MaintenanceResult result) {
  switch (result) {
    case ObservationStore::MaintenanceResult::kStarted: return "started";
    case ObservationStore::MaintenanceResult::kNoWork: return "no_work";
    case ObservationStore::MaintenanceResult::kControlMaintenanceRequired:
      return "control_required";
    case ObservationStore::MaintenanceResult::kBusy: return "busy";
    case ObservationStore::MaintenanceResult::kRejected: return "rejected";
  }
  return "unknown";
}

const char* maintenance_failure = "none";

// Data maintenance, with control maintenance first when the store asks.
// After a reset during control compaction the store resumes that compaction
// and rejects data maintenance until control maintenance finishes it (SF5C
// LOW-1), so a rejection while healthy is answered with control maintenance.
// Returns false on failure (reason in maintenance_failure); `none` is set
// when there was nothing to do.
bool runMaintenance(bool& none) {
  none = false;
  maintenance_failure = "none";
  for (unsigned pass = 0; pass < 4U; ++pass) {
    const ObservationStore::MaintenanceResult result = store.requestMaintenance();
    bool success = false;
    if (result == ObservationStore::MaintenanceResult::kStarted) {
      settle();
      if (store.takeMaintenanceResult(success) && success) return true;
      maintenance_failure = "data_job_failed";
      return false;
    }
    if (result == ObservationStore::MaintenanceResult::kNoWork) {
      none = true;
      return true;
    }
    const bool resume_control =
        result == ObservationStore::MaintenanceResult::kRejected &&
        store.ready() && !store.faulted() && pass == 0U;
    if (result != ObservationStore::MaintenanceResult::kControlMaintenanceRequired &&
        !resume_control) {
      maintenance_failure = maintenanceName(result);
      return false;
    }
    const ObservationStore::MaintenanceResult control =
        store.requestControlMaintenance();
    if (control != ObservationStore::MaintenanceResult::kStarted) {
      maintenance_failure = control == ObservationStore::MaintenanceResult::kNoWork
                                ? "control_no_work"
                                : "control_refused";
      return false;
    }
    settle();
    if (!store.takeControlMaintenanceResult(success) || !success) {
      maintenance_failure = "control_job_failed";
      return false;
    }
  }
  maintenance_failure = "loop";
  return false;
}

enum class AppendOutcome : uint8_t { kDone, kNeedsMaintenance, kFailed };

AppendOutcome appendOne(bool allow_maintenance) {
  osf::RecordIdentity id{};
  if (!store.peekNextIdentity(id)) return AppendOutcome::kFailed;
  uint8_t payload[kPayloadSize];
  for (unsigned i = 0; i < kPayloadSize; ++i) payload[i] = patternByte(id.sequence, i);

  ObservationStore::AppendResult result = store.requestAppend(
      osf::RecordKind::kPeriodic, osf::kProductSchemaV1, payload, kPayloadSize);
  // No capacity, or a maintenance step still resuming after a reset.
  if (result == ObservationStore::AppendResult::kNoCapacity ||
      result == ObservationStore::AppendResult::kRejected) {
    if (!allow_maintenance) return AppendOutcome::kNeedsMaintenance;
    bool none = false;
    if (!runMaintenance(none)) return AppendOutcome::kFailed;
    result = store.requestAppend(osf::RecordKind::kPeriodic,
                                 osf::kProductSchemaV1, payload, kPayloadSize);
  }
  if (result != ObservationStore::AppendResult::kStarted)
    return AppendOutcome::kFailed;
  settle();
  bool success = false;
  ObservationStore::Handle handle;
  return store.takeAppendResult(success, handle) && success
             ? AppendOutcome::kDone
             : AppendOutcome::kFailed;
}

void commandAppend(uint32_t count, bool fill_page) {
  const uint32_t calls_before = cut_backend.calls();
  uint32_t done = 0;
  AppendOutcome outcome = AppendOutcome::kDone;
  for (uint32_t i = 0; i < count; ++i) {
    feedWatchdog();
    outcome = appendOne(!fill_page);
    if (outcome != AppendOutcome::kDone) break;
    ++done;
  }
  const bool ok = fill_page ? outcome == AppendOutcome::kNeedsMaintenance
                            : outcome == AppendOutcome::kDone;
  Serial.printf("SF5E %s done=%lu calls=%lu result=%s\n",
                fill_page ? "FILLPAGE" : "APPEND",
                static_cast<unsigned long>(done),
                static_cast<unsigned long>(cut_backend.calls() - calls_before),
                ok ? "PASS" : "FAIL");
  printStore();
}

void commandMaintenance() {
  const uint32_t calls_before = cut_backend.calls();
  bool none = false;
  const bool ok = runMaintenance(none);
  Serial.printf("SF5E MAINT calls=%lu work=%s reason=%s result=%s\n",
                static_cast<unsigned long>(cut_backend.calls() - calls_before),
                none ? "none" : "done", maintenance_failure,
                ok ? "PASS" : "FAIL");
  printStore();
}

void commandRelease(uint32_t count) {
  const uint32_t calls_before = cut_backend.calls();
  uint32_t done = 0;
  bool ok = true;
  for (uint32_t i = 0; i < count && ok; ++i) {
    feedWatchdog();
    ObservationStore::Record record;
    ok = store.oldestRetained(record) == ObservationStore::LookupResult::kFound &&
         store.requestRelease(record.handle.identity);
    if (!ok) break;
    settle();
    bool success = false;
    ok = store.takeReleaseResult(success) && success;
    if (ok) ++done;
  }
  Serial.printf("SF5E RELEASE done=%lu calls=%lu result=%s\n",
                static_cast<unsigned long>(done),
                static_cast<unsigned long>(cut_backend.calls() - calls_before),
                ok ? "PASS" : "FAIL");
  printStore();
}

int compareU32(const void* a, const void* b) {
  const uint32_t x = *static_cast<const uint32_t*>(a);
  const uint32_t y = *static_cast<const uint32_t*>(b);
  return x < y ? -1 : (x > y ? 1 : 0);
}

// Raw page walk: every committed record must carry its exact pattern, no
// sequence may appear twice, and the committed sequences must be one
// contiguous run (older pages are only ever reclaimed from the front).
void commandVerify() {
  uint32_t count = 0, retained = 0, released = 0, torn = 0, bad = 0;
  uint32_t active_pages = 0, other_pages = 0;
  const uint64_t incarnation = store.incarnation();
  for (uint32_t page = osf::kControlPageCount; page < kProductRegionPages;
       ++page) {
    feedWatchdog();
    osf::PageInspection header;
    if (!gate.productPort().read(page * kPageSize, header_bytes,
                                 sizeof(header_bytes)) ||
        !osf::inspectPageHeader(header_bytes, sizeof(header_bytes), header)) {
      ++bad;
      continue;
    }
    if (header.evidence == osf::PageEvidence::kErased) continue;
    if (header.evidence != osf::PageEvidence::kActive) {
      if (header.evidence == osf::PageEvidence::kCorrupt ||
          header.evidence == osf::PageEvidence::kUnsupported)
        ++bad;
      else
        ++other_pages;
      continue;
    }
    if (header.kind != osf::PageKind::kData || header.device_id != device_id ||
        (mounted && header.incarnation != incarnation)) {
      ++bad;
      continue;
    }
    ++active_pages;
    for (uint32_t slot = 0; slot < osf::kDataRecordsPerPage; ++slot) {
      osf::RecordInspection record;
      const uint32_t offset =
          page * kPageSize + osf::kPageHeaderSize + slot * osf::kDataRecordSize;
      if (!gate.productPort().read(offset, record_bytes, sizeof(record_bytes)) ||
          !osf::inspectRecord(record_bytes, sizeof(record_bytes), record)) {
        ++bad;
        continue;
      }
      if (record.evidence == osf::RecordEvidence::kErased) continue;
      if (record.evidence == osf::RecordEvidence::kStaged ||
          record.evidence == osf::RecordEvidence::kPartialCommit) {
        ++torn;
        continue;
      }
      if (record.evidence == osf::RecordEvidence::kCorrupt) {
        ++bad;
        continue;
      }
      bool payload_ok = record.payload_size == kPayloadSize &&
                        record.incarnation == header.incarnation;
      for (unsigned i = 0; payload_ok && i < kPayloadSize; ++i)
        payload_ok = record.payload[i] == patternByte(record.sequence, i);
      if (!payload_ok || count >= kMaxRecords) {
        ++bad;
        continue;
      }
      verify_sequences[count++] = record.sequence;
      if (record.evidence == osf::RecordEvidence::kReleased) ++released;
      else ++retained;
    }
  }

  qsort(verify_sequences, count, sizeof(verify_sequences[0]), compareU32);
  uint32_t duplicates = 0;
  for (uint32_t i = 1; i < count; ++i)
    if (verify_sequences[i] == verify_sequences[i - 1]) ++duplicates;
  const uint32_t first = count ? verify_sequences[0] : 0;
  const uint32_t last = count ? verify_sequences[count - 1] : 0;
  const uint32_t holes =
      count ? (last - first + 1U) - (count - duplicates) : 0U;
  const bool pass = bad == 0 && duplicates == 0 && holes == 0;
  Serial.printf(
      "SF5E VERIFY n=%lu ret=%lu rel=%lu torn=%lu bad=%lu dup=%lu holes=%lu "
      "first=%lu last=%lu pages=%lu other=%lu result=%s\n",
      static_cast<unsigned long>(count), static_cast<unsigned long>(retained),
      static_cast<unsigned long>(released), static_cast<unsigned long>(torn),
      static_cast<unsigned long>(bad), static_cast<unsigned long>(duplicates),
      static_cast<unsigned long>(holes), static_cast<unsigned long>(first),
      static_cast<unsigned long>(last),
      static_cast<unsigned long>(active_pages),
      static_cast<unsigned long>(other_pages), pass ? "PASS" : "FAIL");
}

void commandPrepare() {
  // The only whole-region erase. Development device, explicit command.
  bool ok = gate.productPort().begin();
  for (uint32_t page = 0; ok && page < kProductRegionPages; ++page) {
    feedWatchdog();
    ok = gate.productPort().erasePage(page) == FlashOpResult::kDone;
  }
  Serial.printf("SF5E PREPARE erase=%s\n", ok ? "PASS" : "FAIL");
  mount();
  Serial.printf("SF5E PREPARE result=%s\n",
                ok && mounted && last_decision == pr::MountDecision::kMount
                    ? "PASS"
                    : "FAIL");
  printStore();
}

bool parseCount(const char* text, uint32_t& value) {
  char* end = nullptr;
  const unsigned long parsed = strtoul(text, &end, 10);
  if (end == text || *end != '\0' || parsed == 0 || parsed > 5000UL) return false;
  value = static_cast<uint32_t>(parsed);
  return true;
}

void handleCommand() {
  uint32_t value = 0;
  // An armed cut applies to the next command only; if that command finishes
  // without reaching the armed call, say so and disarm.
  const bool cut_was_armed = cut_backend.armed();
  const bool is_cut_command = strncmp(command, "SF5E CUT ", 9) == 0;
  if (strcmp(command, "SF5E STATUS") == 0) {
    printStatus();
  } else if (strcmp(command, "SF5E PREPARE CONFIRM") == 0) {
    commandPrepare();
  } else if (strcmp(command, "SF5E REBOOT") == 0) {
    Serial.println(F("SF5E REBOOT"));
    Serial.flush();
    delay(100);
    NVIC_SystemReset();
  } else if (!mounted) {
    Serial.println(F("SF5E rejected: store not mounted (STATUS / PREPARE CONFIRM)"));
  } else if (strcmp(command, "SF5E VERIFY") == 0) {
    commandVerify();
  } else if (strcmp(command, "SF5E MAINT") == 0) {
    commandMaintenance();
  } else if (strcmp(command, "SF5E FILLPAGE") == 0) {
    // Append until the store needs maintenance: the rest of the active page
    // plus an already prepared page, at most two pages.
    commandAppend(2U * osf::kDataRecordsPerPage + 1U, true);
  } else if (strncmp(command, "SF5E APPEND ", 12) == 0 &&
             parseCount(command + 12, value)) {
    commandAppend(value, false);
  } else if (strncmp(command, "SF5E RELEASE ", 13) == 0 &&
             parseCount(command + 13, value)) {
    commandRelease(value);
  } else if (strncmp(command, "SF5E CUT ", 9) == 0) {
    char* end = nullptr;
    const unsigned long call = strtoul(command + 9, &end, 10);
    CutMode mode = CutMode::kNone;
    if (end != nullptr && *end == ' ') {
      const char* name = end + 1;
      if (strcmp(name, "BEFORE") == 0) mode = CutMode::kBefore;
      else if (strcmp(name, "TORN") == 0) mode = CutMode::kTorn;
      else if (strcmp(name, "AFTER") == 0) mode = CutMode::kAfter;
      else if (strcmp(name, "HOLD") == 0) mode = CutMode::kHold;
    }
    if (call == 0 || call > 64 || mode == CutMode::kNone) {
      Serial.println(F("SF5E CUT rejected: SF5E CUT <1..64> <BEFORE|TORN|AFTER|HOLD>"));
    } else {
      cut_backend.arm(static_cast<uint32_t>(call), mode);
      Serial.printf("SF5E CUT armed call=%lu mode=%s\n", call, cutModeName(mode));
    }
  } else {
    Serial.println(F("SF5E rejected: STATUS VERIFY APPEND <n> FILLPAGE MAINT "
                     "RELEASE <n> CUT <k> <mode> REBOOT PREPARE CONFIRM"));
  }
  if (cut_was_armed && !is_cut_command && cut_backend.armed()) {
    cut_backend.disarm();
    Serial.println(F("SF5E CUT not_reached; disarmed"));
  }
  Serial.flush();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t started = millis();
  while (!Serial && (millis() - started) < 15000U) {
    feedWatchdog();
    delay(10);
  }
  device_id = (uint64_t(NRF_FICR->DEVICEID[1]) << 32) | NRF_FICR->DEVICEID[0];
  if (device_id == 0U) device_id = 1U;
  Serial.println(F("SF5E OBSERVATION QUAL TEST-ONLY; boot never mutates"));
  mount();
  printStatus();
  Serial.println(F("SF5E READY"));
  Serial.flush();
}

void loop() {
  feedWatchdog();
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      if (command_length == 0) continue;
      command[command_length] = '\0';
      handleCommand();
      command_length = 0;
      continue;
    }
    if (command_length < sizeof(command) - 1U) {
      command[command_length++] = static_cast<char>(c);
    } else {
      command_length = 0;
      Serial.println(F("SF5E rejected: command too long"));
    }
  }
  delay(5);
}
