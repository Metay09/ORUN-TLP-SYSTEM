#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

#include "accelerometer_manager.h"
#include "activity_capture.h"
#include "ble_admission_policy.h"
#include "config_store.h"
#include "firmware_version.h"
#include "flash_mutation_gate.h"
#include "gnss_manager.h"
#include "node_role.h"
#include "power_manager.h"
#include "radio_manager.h"
#include "position_flow.h"
#include "monotonic_time.h"
#include "rak_device_identity.h"
#include "runtime_config.h"
#include "security_store.h"
#include "sensor_power_manager.h"
#include "watchdog_manager.h"

namespace {

orun_tlp::RadioManager radio_manager;
orun_tlp::GnssManager gnss_manager;
orun_tlp::AccelerometerManager accelerometer_manager;
orun_tlp::ActivityCapture activity_capture(accelerometer_manager);
// M7P3: FlashMutationGate wraps NrfHistoryFlash unchanged for the
// SoftDevice-disabled path (still the only path exercised by shipped
// firmware); its asynchronous path is not enabled by anything in this
// runtime (BLE is not started). M7P5 generalized it to also serve
// ConfigStore through configPort() (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md
// §9/§10); the history-facing API/behavior used below is unchanged.
orun_tlp::FlashMutationGate storage_flash_gate;
orun_tlp::HistoryStore history(storage_flash_gate);
orun_tlp::ConfigStore config_store(storage_flash_gate.configPort());
// M7P6B: recovery-only composition. SecurityStore never auto-provisions a
// credential in production firmware -- begin() only recovers whatever
// already exists (or reports kUnprovisioned on blank flash). A recovered
// PROVISIONED store deliberately starts one fresh TX reservation at boot so
// the next secure counter skips all possibly-used counters from the prior
// reserved block; poll() advances that bounded recovery reservation.
// Current production has no provisioning path, so blank devices remain
// UNPROVISIONED and perform no security writes. This proves the real
// instantiated object's RAM/flash footprint and preserves TLP v1/RF/GNSS/
// role behavior without implementing crypto, secure envelope, commands or BLE.
orun_tlp::SecurityStore security_store(storage_flash_gate.securityCriticalPort(),
                                       storage_flash_gate.securityMaintPort());
orun_tlp::PositionFlow positions(history, radio_manager);
orun_tlp::RoleController role_controller;
bool automatic_role_resolved = false;
char role_command[24]{};
uint8_t role_command_length = 0;
bool role_command_overflow = false;

// M7P7B: BLE runtime/admission. BleAdmissionPolicy is the pure, host-tested
// tracker no-client-timeout decision (docs/architecture/
// ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md §10); this composition root is the
// only place that touches Bluefruit itself. ble_ready is false until
// Bluefruit.begin() succeeds, guarding every later Bluefruit call the same
// way radio_manager's own begin() result already guards radio use below.
orun_tlp::BleAdmissionPolicy ble_admission;
bool ble_ready = false;
// Result of the one boot-time Bluefruit.Advertising.start(0) call, kept apart
// from ble_ready (SoftDevice/Bluefruit runtime readiness) so BLE? can tell
// "runtime up but advertising never started" from "advertising running".
enum class BleInitialStart : uint8_t { kNotAttempted, kOk, kFail };
BleInitialStart ble_initial_start = BleInitialStart::kNotAttempted;
// Cross-task disconnect handoff (audit finding 1). Adafruit nRF52 1.7.0 runs
// Periph's disconnect callback on its own "Callback" FreeRTOS task
// (ada_callback), never on loop(). That callback does exactly one thing:
// increment this counter inside taskENTER/EXIT_CRITICAL -- the same
// primitive radio_manager.cpp uses for its cross-task counters. loop() is the
// only reader/consumer and owns every policy, clock, Serial and Bluefruit
// action; ble_disconnect_events_seen is loop-task-only.
volatile uint32_t ble_disconnect_events = 0;
uint32_t ble_disconnect_events_seen = 0;
// Loop-task-only: suppresses per-retry log spam while restart keeps failing.
bool ble_restart_failing = false;

// Bluefruit "Callback"-task context. MUST NOT call monotonic::nowMs(),
// BleAdmissionPolicy, Serial, flash, radio or Bluefruit/SoftDevice APIs.
void onBleDisconnect(uint16_t, uint8_t) {
  taskENTER_CRITICAL();
  ++ble_disconnect_events;
  taskEXIT_CRITICAL();
}

enum class AccelerometerDiagnosticState : uint8_t {
  kPending,
  kPresent,
  kAbsent,
  kFault,
};

AccelerometerDiagnosticState accelerometer_diagnostic_state =
    AccelerometerDiagnosticState::kPending;
orun_tlp::AccelerometerSample accelerometer_diagnostic_sample{};
bool accelerometer_diagnostic_sample_valid = false;

void applyRole(orun_tlp::NodeRole role, const char* source) {
  radio_manager.setRole(role);
  Serial.printf("ROLE %s source=%s\n", orun_tlp::roleName(role), source);
}

bool isAccelerometerQuery() {
  static const char kQuery[] = "ACCEL?";
  constexpr uint8_t kQueryLength = sizeof(kQuery) - 1;
  if (role_command_length != kQueryLength) return false;
  for (uint8_t i = 0; i < kQueryLength; ++i) {
    if (role_command[i] != kQuery[i]) return false;
  }
  return true;
}

void printAccelerometerDiagnostic() {
  if (accelerometer_diagnostic_state == AccelerometerDiagnosticState::kPending) {
    Serial.println(F("ACCEL PENDING"));
    return;
  }
  if (accelerometer_diagnostic_state == AccelerometerDiagnosticState::kPresent) {
    if (accelerometer_diagnostic_sample_valid) {
      Serial.printf("ACCEL PRESENT x_mg=%d y_mg=%d z_mg=%d\n",
                    static_cast<int>(accelerometer_diagnostic_sample.x_mg),
                    static_cast<int>(accelerometer_diagnostic_sample.y_mg),
                    static_cast<int>(accelerometer_diagnostic_sample.z_mg));
    } else {
      Serial.println(F("ACCEL PRESENT"));
    }
    return;
  }
  if (accelerometer_diagnostic_state == AccelerometerDiagnosticState::kAbsent) {
    Serial.println(F("ACCEL ABSENT"));
    return;
  }
  Serial.printf("ACCEL FAULT presence=%s\n",
                accelerometer_manager.detected() ? "PRESENT" : "UNKNOWN");
}

bool isActivityCommand(const char* text, uint8_t length) {
  if (role_command_length != length) return false;
  for (uint8_t i = 0; i < length; ++i)
    if (role_command[i] != text[i]) return false;
  return true;
}

void printActivityDiagnostic() {
  using State = orun_tlp::ActivityCapture::State;
  const auto state = activity_capture.state();
  const auto* features = activity_capture.result();
  if (features != nullptr) {
    Serial.printf("ACTIVITY %s samples=%u duration_ms=%lu discontinuities=%u "
                  "usable=%s mean_x_mg=%ld mean_y_mg=%ld mean_z_mg=%ld "
                  "axis_variance_sum_mg2=%lu mean_magnitude_squared_mg2=%lu "
                  "mean_abs_delta_mg=%lu\n",
                  state == State::kReady ? "READY" : "INVALID",
                  static_cast<unsigned>(features->sample_count),
                  static_cast<unsigned long>(features->duration_ms),
                  static_cast<unsigned>(features->timing_discontinuities),
                  activity_capture.assessment().usable ? "yes" : "no",
                  static_cast<long>(features->mean_x_mg),
                  static_cast<long>(features->mean_y_mg),
                  static_cast<long>(features->mean_z_mg),
                  static_cast<unsigned long>(features->axis_variance_sum_mg2),
                  static_cast<unsigned long>(features->mean_magnitude_squared_mg2),
                  static_cast<unsigned long>(features->mean_abs_delta_mg));
  } else if (state == State::kFault || accelerometer_manager.faulted()) {
    Serial.println(F("ACTIVITY UNAVAILABLE accel=FAULT"));
  } else if (!accelerometer_manager.detectionComplete()) {
    Serial.println(F("ACTIVITY UNAVAILABLE accel=PENDING"));
  } else if (!accelerometer_manager.detected()) {
    Serial.println(F("ACTIVITY UNAVAILABLE accel=ABSENT"));
  } else if (state == State::kCapturing) {
    Serial.printf("ACTIVITY CAPTURING samples=%u\n",
                  static_cast<unsigned>(activity_capture.sampleCount()));
  } else if (state == State::kStopping) {
    Serial.println(F("ACTIVITY STOPPING"));
  } else {
    Serial.println(F("ACTIVITY IDLE"));
  }
}

void startActivityCapture() {
  using Result = orun_tlp::ActivityCapture::StartResult;
  const auto result = activity_capture.start();
  if (result == Result::kStarted) {
    printActivityDiagnostic();
    return;
  }
  const char* reason = result == Result::kPending ? "PENDING" :
                       result == Result::kAbsent ? "ABSENT" :
                       result == Result::kFault ? "FAULT" : "BUSY";
  Serial.printf("ACTIVITY START rejected: %s\n", reason);
}

void printRadioDiagnostic() {
  const auto diagnostics = radio_manager.listenDiagnostics();
  Serial.printf("RADIO policy=%s state=%s windows=%lu sleeps=%lu wakes=%lu "
                "rx_in_window=%lu stale=%lu rx_ms_est=%lu\n",
                orun_tlp::radioListenPolicyName(diagnostics.listen_policy),
                orun_tlp::radioListenStateName(diagnostics.listen_state),
                static_cast<unsigned long>(diagnostics.windows_opened),
                static_cast<unsigned long>(diagnostics.sleep_entries),
                static_cast<unsigned long>(diagnostics.wakes_for_tx),
                static_cast<unsigned long>(diagnostics.rx_events_in_window),
                static_cast<unsigned long>(diagnostics.stale_restores_while_asleep),
                static_cast<unsigned long>(diagnostics.estimated_rx_ms));
}

void printBleDiagnostic() {
  const char* initial_start = "not-attempted";
  if (ble_initial_start == BleInitialStart::kOk) initial_start = "ok";
  else if (ble_initial_start == BleInitialStart::kFail) initial_start = "fail";
  // Bluefruit state is only read once Bluefruit.begin() has succeeded.
  const bool advertising = ble_ready && Bluefruit.Advertising.isRunning();
  const unsigned connected =
      ble_ready ? static_cast<unsigned>(Bluefruit.Periph.connected()) : 0U;
  Serial.printf("BLE ready=%s advertising=%s connected=%u policy=%s "
                "initial_start=%s\n",
                ble_ready ? "yes" : "no", advertising ? "yes" : "no", connected,
                ble_admission.isOpen()      ? "open"
                : ble_admission.isClosing() ? "closing"
                                            : "closed",
                initial_start);
}

void handleRoleCommand() {
  if (role_command_overflow) {
    role_command_length = 0;
    role_command_overflow = false;
    Serial.println(F("ROLE command rejected: too long"));
    return;
  }
  if (isAccelerometerQuery()) {
    role_command_length = 0;
    printAccelerometerDiagnostic();
    return;
  }

  if (isActivityCommand("RADIO?", 6)) {
    role_command_length = 0;
    printRadioDiagnostic();
    return;
  }
  if (isActivityCommand("BLE?", 4)) {
    role_command_length = 0;
    printBleDiagnostic();
    return;
  }
  if (isActivityCommand("ACTIVITY?", 9)) {
    role_command_length = 0;
    printActivityDiagnostic();
    return;
  }
  if (isActivityCommand("ACTIVITY START", 14)) {
    role_command_length = 0;
    startActivityCapture();
    return;
  }

  const auto command = orun_tlp::parseRoleCommand(role_command,
                                                   role_command_length);
  role_command_length = 0;
  if (command == orun_tlp::RoleCommand::kQuery) {
    Serial.printf("ROLE %s mode=%s\n", orun_tlp::roleName(role_controller.role()),
                  role_controller.automatic() ? "AUTO" : "OVERRIDE");
    return;
  }
  orun_tlp::NodeRole role;
  if (command == orun_tlp::RoleCommand::kTracker)
    role = orun_tlp::NodeRole::kTracker;
  else if (command == orun_tlp::RoleCommand::kRelay)
    role = orun_tlp::NodeRole::kRelay;
  else if (command == orun_tlp::RoleCommand::kBase)
    role = orun_tlp::NodeRole::kBase;
  else if (command == orun_tlp::RoleCommand::kNone)
    return;
  else {
    Serial.println(F("ROLE command rejected"));
    return;
  }
  role_controller.applyOverride(role);
  automatic_role_resolved = true;
  applyRole(role, "USB-OVERRIDE");
}

void pollRoleCommands() {
  constexpr uint8_t kMaximumBytesPerLoop = 32;
  for (uint8_t count = 0; count < kMaximumBytesPerLoop && Serial.available();
       ++count) {
    const int input = Serial.read();
    if (input < 0) break;
    const char value = static_cast<char>(input);
    if (value == '\r' || value == '\n') {
      handleRoleCommand();
    } else if (!role_command_overflow) {
      if (static_cast<size_t>(role_command_length + 1) < sizeof(role_command))
        role_command[role_command_length++] = value;
      else
        role_command_overflow = true;
    }
  }
}

orun_tlp::CapabilitySnapshot currentCapabilitySnapshot() {
  // Firmware support and physical presence are separate facts. The current
  // RAK product image supports GNSS and the owned RAK1904/LIS3DH path, while
  // bounded detection owns whether optional hardware is presently known to
  // exist. Neither capability is allowed to infer role/profile.
  orun_tlp::CapabilityState gnss(
      true, orun_tlp::CapabilityPresence::kUnknown,
      orun_tlp::CapabilityHealth::kUnavailable);
  if (gnss_manager.detectionComplete()) {
    if (gnss_manager.detected()) {
      gnss.presence = orun_tlp::CapabilityPresence::kPresent;
      // Detection proves the device is responsive enough to enter the existing
      // GNSS state machine. Acquisition-specific failures remain owned there;
      // they do not make installed hardware disappear.
      gnss.health = orun_tlp::CapabilityHealth::kOk;
    } else {
      gnss.presence = orun_tlp::CapabilityPresence::kAbsent;
      gnss.health = orun_tlp::CapabilityHealth::kUnavailable;
    }
  }

  orun_tlp::CapabilityState accelerometer(
      true, orun_tlp::CapabilityPresence::kUnknown,
      orun_tlp::CapabilityHealth::kUnavailable);
  if (accelerometer_manager.detectionComplete()) {
    if (accelerometer_manager.detected()) {
      accelerometer.presence = orun_tlp::CapabilityPresence::kPresent;
      accelerometer.health = accelerometer_manager.faulted()
                                 ? orun_tlp::CapabilityHealth::kFault
                                 : orun_tlp::CapabilityHealth::kOk;
    } else if (accelerometer_manager.faulted()) {
      // A transport/recovery failure before positive WHO_AM_I is not proof that
      // the physical module is absent.
      accelerometer.presence = orun_tlp::CapabilityPresence::kUnknown;
      accelerometer.health = orun_tlp::CapabilityHealth::kFault;
    } else {
      accelerometer.presence = orun_tlp::CapabilityPresence::kAbsent;
      accelerometer.health = orun_tlp::CapabilityHealth::kUnavailable;
    }
  }

  return orun_tlp::CapabilitySnapshot(gnss, accelerometer);
}

bool serviceRuns(const orun_tlp::ServiceStatus& status) {
  return status.state == orun_tlp::ServiceState::kEnabled ||
         status.state == orun_tlp::ServiceState::kDegraded;
}

orun_tlp::EffectiveConfig resolveRuntimeConfig() {
  // B4 still uses the frozen legacy role projection as the requested defaults.
  // A later validated config surface can replace this source without returning
  // runtime ownership to NodeRole. M6A adds only hardware capability discovery;
  // it does not invent an activity requested-config field yet.
  const auto requested = orun_tlp::requestedConfigFromLegacyBehavior(
      orun_tlp::legacyRoleBehavior(role_controller.role()));
  return orun_tlp::resolveRequestedConfig(requested, currentCapabilitySnapshot());
}

void handleAccelerometerEvent(orun_tlp::AccelerometerManager::Event event) {
  if (event == orun_tlp::AccelerometerManager::Event::kPresent) {
    accelerometer_diagnostic_state = AccelerometerDiagnosticState::kPresent;
    orun_tlp::AccelerometerSample sample{};
    if (accelerometer_manager.takeProbeSample(&sample)) {
      accelerometer_diagnostic_sample = sample;
      accelerometer_diagnostic_sample_valid = true;
      Serial.printf("ACCEL PRESENT x_mg=%d y_mg=%d z_mg=%d\n",
                    static_cast<int>(sample.x_mg),
                    static_cast<int>(sample.y_mg),
                    static_cast<int>(sample.z_mg));
    } else {
      accelerometer_diagnostic_sample_valid = false;
      Serial.println(F("ACCEL PRESENT"));
    }
  } else if (event == orun_tlp::AccelerometerManager::Event::kAbsent) {
    accelerometer_diagnostic_state = AccelerometerDiagnosticState::kAbsent;
    accelerometer_diagnostic_sample_valid = false;
    Serial.println(F("ACCEL ABSENT"));
  } else if (event == orun_tlp::AccelerometerManager::Event::kFault) {
    accelerometer_diagnostic_state = AccelerometerDiagnosticState::kFault;
    accelerometer_diagnostic_sample_valid = false;
    Serial.printf("ACCEL FAULT presence=%s\n",
                  accelerometer_manager.detected() ? "PRESENT" : "UNKNOWN");
  }
}

void printBootBanner() {
  Serial.println(F("ORUN TLP"));
  Serial.print(F("firmware version "));
  Serial.println(orun_tlp::kFirmwareVersion);
  Serial.print(F("build date/time "));
  Serial.print(F(__DATE__));
  Serial.print(F(" "));
  Serial.println(F(__TIME__));
  const auto& reset = orun_tlp::WatchdogManager::bootInfo();
  Serial.printf("RESET reason=0x%08lx watchdog=%s\n",
                static_cast<unsigned long>(reset.reset_reason),
                reset.watchdog_reset ? "yes" : "no");
  Serial.println(F("M0 BOOT OK"));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // Start the hardware watchdog before peripheral initialization. It is the
  // final recovery layer if bounded driver recovery itself cannot make progress.
  orun_tlp::WatchdogManager::begin();
  printBootBanner();
  orun_tlp::SensorPowerManager::begin();

  // Identity is a board capability, not a radio side effect. Resolve it before
  // radio startup so storage recovery and packet mapping survive radio failure.
  const orun_tlp::DeviceIdentity device_identity =
      orun_tlp::RakDeviceIdentityProvider{}.read();
  radio_manager.setDeviceIdentity(device_identity);
  positions.setDeviceIdentity(device_identity);

  if (!radio_manager.begin(history)) {
    Serial.println(F("RADIO unavailable; TX/RX disabled; local services continue"));
  }
  radio_manager.setRole(role_controller.role());
  if (history.begin(device_identity.legacyUint64())) {
    Serial.printf("STORAGE recovered records=%lu capacity=%lu corrupt=%lu pending=%lu\n",
                  static_cast<unsigned long>(history.count()),
                  static_cast<unsigned long>(history.capacity()),
                  static_cast<unsigned long>(history.diagnostics().recovery_corruptions),
                  static_cast<unsigned long>(history.backlogCount()));
  } else Serial.println(F("STORAGE unavailable; POSITION TX disabled"));
  // M7P5: recover durable config before GNSS starts, so the very first
  // acquisition schedule already reflects it. config_store.config() reads
  // the safe 180s/unspecified-battery default on blank flash, a corrupt
  // page, or a begin() failure -- see ConfigStore::begin()'s contract.
  if (!config_store.begin()) {
    Serial.println(F("CONFIG unavailable; defaults in effect"));
  }
  // M7P6B: recovery only -- never provisions a credential. See the
  // composition-root comment on security_store above.
  if (!security_store.begin(device_identity)) {
    Serial.println(F("SECURITY unavailable"));
  } else {
    const char* state = "UNKNOWN";
    switch (security_store.state()) {
      case orun_tlp::SecurityState::kUnprovisioned: state = "UNPROVISIONED"; break;
      case orun_tlp::SecurityState::kProvisioned: state = "PROVISIONED"; break;
      case orun_tlp::SecurityState::kForeign: state = "FOREIGN"; break;
      case orun_tlp::SecurityState::kUnsupported: state = "UNSUPPORTED"; break;
      case orun_tlp::SecurityState::kFault: state = "FAULT"; break;
    }
    Serial.printf("SECURITY state=%s\n", state);
  }
  gnss_manager.begin();
  gnss_manager.setTrackingIntervalMs(
      config_store.config().tracking_interval_seconds * 1000UL);
  accelerometer_manager.begin(orun_tlp::monotonic::nowMs());
  Serial.println(F("ROLE AUTO pending (GNSS=>TRACKER, no GNSS=>BASE)"));

  // M7P7B: BLE starts last, strictly after History/Config/Security have
  // finished their SoftDevice-disabled synchronous recovery above --
  // NrfHistoryFlash::begin() (and the Config/Security equivalents) fail
  // closed if SoftDevice is already enabled when they run, and
  // Bluefruit.begin() is what enables SoftDevice for the rest of this boot
  // (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md §9: the sync/async
  // backend mode is fixed for the lifetime of one boot, not hot-swapped).
  // No advertising/pairing/provisioning GATT service is added; a bare,
  // named, connectable peripheral is sufficient to prove the M7P7B runtime.
  ble_ready = Bluefruit.begin();
  if (ble_ready) {
    char name[16];
    snprintf(name, sizeof(name), "ORUN-%08lX",
             static_cast<unsigned long>(device_identity.legacyUint64() & 0xFFFFFFFFUL));
    Bluefruit.setName(name);
    // setName() only sets the GAP Device Name attribute, readable after a
    // client connects -- it does not, by itself, put anything into the
    // advertising PDU. Every stock Bluefruit peripheral example calls both
    // of these before Advertising.start(); without them the broadcast
    // payload has zero AD structures, so a scanner sees an anonymous
    // device instead of the name set above. addFlags() marks this as a
    // standard LE-only general-discoverable peripheral; addName() copies
    // the name actually into the advertising data.
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addName();
    // Stock Bluefruit blinks LED_BLUE on a FreeRTOS timer for the entire
    // advertising/connected duration (default _led_conn=true,
    // bluefruit.cpp's _startConnLed()/bluefruit_blinky_cb). That is an
    // avoidable, continuous GPIO toggle this milestone would otherwise
    // introduce on every boot's ~10-minute window; ANIMAL_TRACKER power
    // policy (AGENTS.md) has no use for a connection-status LED, so disable
    // it rather than measure-and-accept it.
    Bluefruit.autoConnLed(false);
    // Pinned 1.7.0 BLEAdvertising::_eventHandler() restarts advertising on
    // disconnect with start(_stop_timeout) and silently ignores its result,
    // so a failed restart would leave the policy open while nothing
    // advertises (audit finding 3). Disable it: loop() owns every
    // post-disconnect Advertising.start(0), checks the result and retries.
    Bluefruit.Advertising.restartOnDisconnect(false);
    // Minimal event handoff only; see onBleDisconnect() above.
    Bluefruit.Periph.setDisconnectCallback(onBleDisconnect);
    // No library-owned timeout (0): BleAdmissionPolicy owns the only close
    // deadline that matters, driven from loop() below.
    const bool advertising_started = Bluefruit.Advertising.start(0);
    ble_initial_start =
        advertising_started ? BleInitialStart::kOk : BleInitialStart::kFail;
    if (advertising_started) {
      ble_admission.begin(orun_tlp::monotonic::nowMs());
      Serial.printf("BLE available name=%s\n", name);
    } else {
      // Runtime is up but nothing is advertising: never open the admission
      // window or claim availability. Query with BLE? for the full state.
      Serial.println(F("BLE advertising start failed"));
    }
  } else {
    Serial.println(F("BLE unavailable"));
  }
}

void loop() {
  // GNSS detection/power remains owned by GnssManager. Service resolution must
  // not silently turn role, location source, GNSS power or accelerometer
  // presence into one knob.
  gnss_manager.poll();
  handleAccelerometerEvent(
      accelerometer_manager.poll(orun_tlp::monotonic::nowMs()));
  activity_capture.poll();
  pollRoleCommands();
  if (!automatic_role_resolved && role_controller.automatic() &&
      gnss_manager.detectionComplete()) {
    role_controller.updateAutomatic(true, gnss_manager.detected());
    automatic_role_resolved = true;
    applyRole(role_controller.role(), "AUTO");
  }

  const auto effective = resolveRuntimeConfig();
  const bool tracking_enabled = serviceRuns(effective.tracking);
  const bool relay_forwarding_enabled = serviceRuns(effective.relay_forwarding);
  // Applying relay behavior may defer while a TX or legacy role transition is
  // active. The loop retries the same resolved intent without aborting work.
  radio_manager.setRelayForwardingEnabled(relay_forwarding_enabled);

  // M7P7B: drive the pure admission policy from the loop task only. Two
  // sources feed it: the polled Bluefruit state (keeps working for a
  // connection that stays up) and the disconnect event counter handed off by
  // onBleDisconnect() (catches a connect+disconnect that fit entirely
  // between two polls). BLE has its own physical radio (nRF52840 2.4GHz),
  // independent of the SX1262 LoRa radio_manager guards below, so this does
  // not need the TX guard.
  if (ble_ready) {
    taskENTER_CRITICAL();
    const uint32_t disconnect_events = ble_disconnect_events;
    taskEXIT_CRITICAL();
    orun_tlp::BleAdmissionInput input;
    input.disconnect_event = disconnect_events != ble_disconnect_events_seen;
    ble_disconnect_events_seen = disconnect_events;
    // Advertising is sampled before the connection: the framework clears
    // _running only after the connection object exists, so "not running"
    // read here can never pair with a stale "not connected".
    input.advertising_running = Bluefruit.Advertising.isRunning();
    input.connected = Bluefruit.Periph.connected() > 0;
    switch (ble_admission.update(input, orun_tlp::monotonic::nowMs())) {
      case orun_tlp::BleAdmissionAction::kClose: {
        // The deadline only REQUESTS close. stop() can fail (pinned 1.7.0
        // leaves _running unchanged when sd_ble_gap_adv_stop() fails, e.g.
        // a connection racing the stop), so trust observed state, not the
        // return value: confirm only when advertising is really not running
        // and nobody is connected; otherwise the policy repeats the request.
        Bluefruit.Advertising.stop();
        const bool still_running = Bluefruit.Advertising.isRunning();
        const bool connected_now = Bluefruit.Periph.connected() > 0;
        if (!still_running && !connected_now) {
          ble_admission.confirmClosed();
          Serial.println(F("BLE closed; no client connected within window"));
        }
        break;
      }
      case orun_tlp::BleAdmissionAction::kStartAdvertising: {
        // Post-disconnect (or failed-earlier) restart. Never start while a
        // client is connected.
        if (Bluefruit.Periph.connected() > 0) break;
        if (Bluefruit.Advertising.start(0)) {
          ble_restart_failing = false;
          Serial.println(F("BLE advertising restarted"));
        } else if (!ble_restart_failing) {
          ble_restart_failing = true;
          Serial.println(F("BLE advertising restart failed; retrying"));
        }
        break;
      }
      case orun_tlp::BleAdmissionAction::kNone:
        break;
    }
  }

  // Drain any SoftDevice flash completion events. Before BLE starts this is
  // a no-op (SoftDevice disabled); once Bluefruit.begin() succeeds above,
  // this consumes the M7P7A-forwarded gate-owned completion mailbox instead
  // of racing Bluefruit's own sd_evt_get() consumption. One shared drain for
  // History, Config (M7P5) and Security (M7P6B) clients.
  storage_flash_gate.pumpEvents();
  // Leave local TX undisturbed; otherwise service one small flash operation.
  // config_store.poll() shares the same TX guard as history.poll() -- a
  // synchronous flash program/erase call must not run while TX is active,
  // for config the same as for history. History polls first: live
  // store-before-send outranks config writes
  // (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md §10), and the shared
  // gate's own admission also enforces this regardless of call order.
  if (!radio_manager.isTransmitting()) {
    history.poll();
    config_store.poll();
    security_store.poll();
  }
  const auto event =
      positions.update(orun_tlp::monotonic::nowMs(), tracking_enabled);
  if (event == orun_tlp::PositionFlow::Event::kStorageFailure)
    Serial.println(F("STORAGE append failed; no live TX"));
  else if (event == orun_tlp::PositionFlow::Event::kStored)
    Serial.printf("STORAGE appended records=%lu overwritten=%lu pending=%lu\n",
                  static_cast<unsigned long>(history.count()),
                  static_cast<unsigned long>(history.diagnostics().overwritten),
                  static_cast<unsigned long>(history.backlogCount()));
  else if (event == orun_tlp::PositionFlow::Event::kLiveExpired)
    Serial.println(F("POSITION live expired; retained in history"));
  orun_tlp::GnssFix fix{};
  if (tracking_enabled && positions.canAcceptFix() &&
      gnss_manager.takeFreshFixForTransmission(&fix)) {
    if (!positions.acceptFix(fix, orun_tlp::monotonic::nowMs()))
      Serial.println(F("STORAGE position dropped; no live TX"));
  }
  radio_manager.update(tracking_enabled && !positions.pending());
  // Feed only after the cooperative loop has completed all service work. A
  // blocked I2C/flash/radio path therefore cannot hide behind an unrelated task.
  orun_tlp::WatchdogManager::feed();
  orun_tlp::PowerManager::idle();
}
