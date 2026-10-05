#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>

#include "accelerometer_manager.h"
#include "activity_capture.h"
#include "application_request.h"
#include "application_status_runtime.h"
#include "ble_admission_policy.h"
#include "ble_application_handoff.h"
#include "ble_application_transport.h"
#include "config_store.h"
#include "config_incarnation_source.h"
#ifdef ORUN_M7P7B_FLASH_PROBE
#include "m7p7b_flash_probe.h"
#include "storage_config.h"
#endif
#ifdef ORUN_M7P6E_CRYPTO_BLE_PROBE
#include "m7p6e_crypto_ble_probe.h"
#include "m7p6e_pairing_evidence.h"
#endif
#ifdef ORUN_M7P6I_HISTORY_CRYPTO_PROBE
#include "m7p6i_history_crypto_probe.h"
#endif
#ifdef ORUN_M4P5C_HISTORY_RECEIPT_PROBE
#include "m4p5c_history_receipt_probe.h"
#endif
#include "firmware_version.h"
#include "flash_mutation_gate.h"
#include "gnss_manager.h"
#include "location_owner.h"
#include "geofence_confirmation_coordinator.h"
#include "geofence_incarnation_source.h"
#include "geofence_runtime_policy.h"
#include "geofence_runtime_provider.h"
#include "geofence_store.h"
#include "node_role.h"
#include "power_manager.h"
#include "radio_manager.h"
#include "position_flow.h"
#include "monotonic_time.h"
#include "rak_device_identity.h"
#include "runtime_config.h"
#include "security_store.h"
#include "sensor_power_manager.h"
#include "tlp_position_packet.h"
#include "usb_application_adapter.h"
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
orun_tlp::NrfHistoryIncarnationSource history_incarnation_source;
orun_tlp::HistoryStore history(storage_flash_gate, &history_incarnation_source);
orun_tlp::NrfConfigIncarnationSource config_incarnation_source;
orun_tlp::ConfigStore config_store(storage_flash_gate.configPort(),
                                   &config_incarnation_source);
orun_tlp::NrfGeofenceIncarnationSource geofence_incarnation_source;
orun_tlp::GeofenceStore geofence_store(storage_flash_gate.geofencePort(),
                                       &geofence_incarnation_source);
// M6D3B's physical qualification measured tight-but-safe loop-task stack
// margins around GeofenceStore::begin(). Keep the record-sized M6D3C boot copy
// out of setup()'s stack frame; this scratch is used only during boot activation.
orun_tlp::geofence_format::Snapshot geofence_boot_snapshot;
// M7P7H: one bounded transport-neutral read snapshot, populated only by the
// loop-owned composition root from existing domain owners. It owns no domain
// truth and contains no driver references.
orun_tlp::ApplicationStatusSnapshot application_status_snapshot{};
// M7P7D/M7P7E: one typed, transport-neutral application request owner.
// M7P7H expands the same owner with bounded read-only status families; USB and
// BLE remain adapters, while future LoRa must bind to this same target-side
// application boundary rather than duplicate domain logic.
orun_tlp::ApplicationRequestService application_requests(
    config_store, &application_status_snapshot);
orun_tlp::BleApplicationTransport ble_application_transport(application_requests);
// Cross-task state is kept in one fixed-memory mailbox owner. Production wraps
// every callback/loop access in taskENTER/EXIT_CRITICAL; the transport itself
// remains loop-owned and is never invoked by a BLE callback.
orun_tlp::BleApplicationHandoff ble_application_handoff;
uint32_t next_usb_application_request_id = 1;
// M7P7H loop-owned snapshot assembly; declared here so the BLE runtime can
// refresh immediately after atomically taking an ingress frame.
void refreshApplicationStatusSnapshot(uint32_t now_ms);
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
// M7P7I: source-neutral RAM owner for the latest accepted Location product
// fact. GNSS is the only producer in this slice. It owns no source-selection
// policy, persistence, transport or GNSS quality state.
orun_tlp::LocationOwner location_owner;
// M6D3C production composition seam. GeofenceStore owns durable semantic
// authority; the M6D2 coordinator owns runtime geometry/confirmation. Boot
// applies only the already-recovered CLEAR/CONFIGURED semantic snapshot across
// the narrow provider below. No BLE/LoRa writer is enabled by this slice.
orun_tlp::GeofenceConfirmationCoordinator geofence_confirmation;
orun_tlp::GnssFix geofence_episode_fixes[
    orun_tlp::geofence_operational_config::kConfirmationObservationLimit]{};
bool geofence_representative_pending = false;
orun_tlp::GnssFix geofence_representative_fix{};
// Applied-at-boot base cadence. Runtime config writes are not applied to GNSS
// mid-session by M7P5, so geofence B/B/3 must derive from the same applied B
// rather than a newer durable value which GNSS has not otherwise adopted.
uint32_t active_tracking_base_interval_seconds =
    orun_tlp::gnss_config::kTrackingIntervalSeconds;
// Slot 0 is also the ordinary scheduled POSITION. If PositionFlow accepted it
// once, a later representative selection of slot 0 must not allocate a second
// sequence/history record for the same physical observation.
bool geofence_slot0_normal_store_expected = false;
bool geofence_slot0_normal_store_accepted = false;
#ifdef ORUN_M6D2_GEOFENCE_PROBE
// TEST-ONLY dynamic fixture. The first accepted GNSS fix becomes the centre of
// a small local rectangle; no production image defines or persists this area.
orun_tlp::GeoPointE7 m6d2_probe_vertices[4]{};
orun_tlp::GeofencePolygonView m6d2_probe_areas[1]{};
bool m6d2_probe_configured = false;
constexpr int32_t kM6D2ProbeHalfSpanE7 = 2500;  // ~20-30 m at field latitudes.
#endif
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
// Cross-task BLE event handoff. Adafruit nRF52 1.7.0 calls the Bluefruit
// global event callback (Bluefruit.setEventCallback) directly from its BLE
// event task at the end of AdafruitBluefruit::_ble_handler(), after framework
// connection/GATT state has been updated and with no ada_callback()/heap
// allocation on that path. Production hands off only bounded facts under the
// same taskENTER/EXIT_CRITICAL primitive radio_manager.cpp uses: the existing
// disconnect counter plus M7P7G's ORUN HVC and terminal GATTS-timeout facts.
// loop() remains the sole owner of admission policy, clocks, Serial,
// application work and
// Bluefruit/SoftDevice actions. Periph's setDisconnectCallback still is not
// used because that path goes through ada_callback and can be dropped/delayed.
// ble_disconnect_events_seen is loop-task-only; "counter != seen" reduces any
// disconnects since the prior tick to one fresh-window event.
volatile uint32_t ble_disconnect_events = 0;
uint32_t ble_disconnect_events_seen = 0;
// Loop-task-only: suppresses per-retry log spam while restart keeps failing.
bool ble_restart_failing = false;

// M7P7G: real ORUN application GATT wiring. UUID identity and wire bytes are
// frozen by M7P7F; these Bluefruit objects are only the physical adapter.
// Bluefruit's uint8_t[16] UUID constructor expects little-endian bytes, so
// setup reverses the canonical RFC4122-order constants into persistent arrays.
BLEService ble_application_service;
BLECharacteristic ble_application_request_characteristic;
BLECharacteristic ble_application_response_characteristic;
uint8_t ble_application_service_uuid_bluefruit[16]{};
uint8_t ble_application_request_uuid_bluefruit[16]{};
uint8_t ble_application_response_uuid_bluefruit[16]{};
uint16_t ble_application_response_value_handle = BLE_GATT_HANDLE_INVALID;
bool ble_application_gatt_ready = false;

// Loop-owned session/indication state. The callback-visible copy lives only in
// BleApplicationHandoff and is updated under the critical section.
bool ble_application_session_active = false;
uint16_t ble_application_connection_handle = BLE_CONN_HANDLE_INVALID;
uint32_t ble_application_session_generation = 0;
bool ble_application_indication_in_flight = false;
constexpr uint32_t kBleApplicationIndicationRetryMs = 25;
uint32_t ble_application_next_indication_attempt_ms = 0;

// A protocol-source BLE_GATTS_EVT_TIMEOUT leaves ATT progress unusable on the
// connection. Recovery is loop-owned: tear down the ORUN application session,
// then request a physical disconnect and retry it at bounded spacing until the
// real disconnect edge is observed. This is terminal-error recovery, NOT an
// inactivity/session-duration policy.
constexpr uint32_t kBleApplicationDisconnectRetryMs = 1000;
bool ble_application_disconnect_pending = false;
uint16_t ble_application_disconnect_handle = BLE_CONN_HANDLE_INVALID;
uint32_t ble_application_next_disconnect_attempt_ms = 0;

#ifdef ORUN_M7P6E_CRYPTO_BLE_PROBE
// M7P6E TEST-ONLY coexistence evidence. The counters are written only by the
// Bluefruit BLE event task and sampled by loop() under the same critical
// section discipline as the disconnect handoff above. No crypto runs in the
// callback.
volatile uint32_t m7p6e_lesc_dhkey_events = 0;
volatile uint32_t m7p6e_auth_status_events = 0;
volatile uint32_t m7p6e_auth_success_events = 0;
volatile uint32_t m7p6e_auth_failure_events = 0;
volatile uint32_t m7p6e_auth_bonded_success_events = 0;
volatile uint32_t m7p6e_auth_lesc_bonded_success_events = 0;
volatile uint32_t m7p6e_sec_update_events = 0;
volatile uint32_t m7p6e_encrypted_sec_update_events = 0;
bool m7p6e_boot_kat_pass = false;

struct M7P6ECryptoStressState {
  bool active = false;
  bool lesc_overlap_seen = false;
  bool pairing_complete_seen = false;
  uint16_t iterations = 0;
  uint16_t lesc_seen_iteration = 0;
  uint16_t pairing_complete_iteration = 0;
  uint32_t next_iteration_ms = 0;
  uint32_t max_kat_us = 0;
  orun_tlp::m7p6e_test::PairingEvidenceCounters evidence_start{};
};

M7P6ECryptoStressState m7p6e_crypto_stress;
#endif

// Bluefruit characteristic write callback runs in the BLE event task because
// useAdaCallback=false is deliberate. It performs one bounded <=20-byte copy
// under the existing critical-section discipline and nothing else: no clock,
// Serial, Bluefruit/SoftDevice call, application/config/flash/radio work.
void onBleApplicationWrite(uint16_t conn_handle, BLECharacteristic* chr,
                           uint8_t* data, uint16_t len) {
  (void)chr;
  taskENTER_CRITICAL();
  (void)ble_application_handoff.enqueueIngress(conn_handle, data, len);
  taskEXIT_CRITICAL();
}

// Bluefruit BLE-event-task context (not loop(), not an ISR). Production
// handling hands off only bounded facts: disconnect count, HVC confirmation
// for the ORUN response value handle, and protocol-source GATTS timeout. The
// M7P6E test-only probe
// additionally reads immutable security-event fields and increments bounded
// counters. This callback MUST NOT call monotonic::nowMs(), BleAdmissionPolicy,
// Serial, flash, radio or Bluefruit/SoftDevice APIs.
void onBleEvent(ble_evt_t* evt) {
#ifdef ORUN_M7P6E_CRYPTO_BLE_PROBE
  if (evt->header.evt_id == BLE_GAP_EVT_LESC_DHKEY_REQUEST ||
      evt->header.evt_id == BLE_GAP_EVT_AUTH_STATUS ||
      evt->header.evt_id == BLE_GAP_EVT_CONN_SEC_UPDATE) {
    taskENTER_CRITICAL();
    if (evt->header.evt_id == BLE_GAP_EVT_LESC_DHKEY_REQUEST) {
      ++m7p6e_lesc_dhkey_events;
    } else if (evt->header.evt_id == BLE_GAP_EVT_AUTH_STATUS) {
      ++m7p6e_auth_status_events;
      const auto& auth = evt->evt.gap_evt.params.auth_status;
      if (auth.auth_status == BLE_GAP_SEC_STATUS_SUCCESS) {
        ++m7p6e_auth_success_events;
        if (auth.bonded) {
          ++m7p6e_auth_bonded_success_events;
          if (auth.lesc) ++m7p6e_auth_lesc_bonded_success_events;
        }
      } else {
        ++m7p6e_auth_failure_events;
      }
    } else {
      ++m7p6e_sec_update_events;
      const auto& mode =
          evt->evt.gap_evt.params.conn_sec_update.conn_sec.sec_mode;
      if (mode.sm == 1U && mode.lv >= 2U) {
        ++m7p6e_encrypted_sec_update_events;
      }
    }
    taskEXIT_CRITICAL();
  }
#endif
  if (evt->header.evt_id == BLE_GATTS_EVT_HVC &&
      evt->evt.gatts_evt.params.hvc.handle ==
          ble_application_response_value_handle) {
    taskENTER_CRITICAL();
    (void)ble_application_handoff.enqueueConfirmation(
        evt->evt.gatts_evt.conn_handle,
        evt->evt.gatts_evt.params.hvc.handle);
    taskEXIT_CRITICAL();
  }

  if (evt->header.evt_id == BLE_GATTS_EVT_TIMEOUT &&
      evt->evt.gatts_evt.params.timeout.src ==
          BLE_GATT_TIMEOUT_SRC_PROTOCOL) {
    taskENTER_CRITICAL();
    (void)ble_application_handoff.enqueueGattTimeout(
        evt->evt.gatts_evt.conn_handle);
    taskEXIT_CRITICAL();
  }

  if (evt->header.evt_id == BLE_GAP_EVT_DISCONNECTED) {
    taskENTER_CRITICAL();
    ++ble_disconnect_events;
    taskEXIT_CRITICAL();
  }
}

void reverseBleUuid128(const uint8_t source[16], uint8_t destination[16]) {
  for (uint8_t i = 0; i < 16; ++i) destination[i] = source[15U - i];
}

bool beginBleApplicationGatt() {
  using namespace orun_tlp::ble_app_transport;

  reverseBleUuid128(kServiceUuid128, ble_application_service_uuid_bluefruit);
  reverseBleUuid128(kRequestCharacteristicUuid128,
                    ble_application_request_uuid_bluefruit);
  reverseBleUuid128(kResponseCharacteristicUuid128,
                    ble_application_response_uuid_bluefruit);

  ble_application_service.setUuid(
      BLEUuid(ble_application_service_uuid_bluefruit));
  if (ble_application_service.begin() != ERROR_NONE) return false;

  ble_application_request_characteristic.setUuid(
      BLEUuid(ble_application_request_uuid_bluefruit));
  ble_application_request_characteristic.setProperties(CHR_PROPS_WRITE);
  ble_application_request_characteristic.setPermission(SECMODE_NO_ACCESS,
                                                       SECMODE_OPEN);
  ble_application_request_characteristic.setMaxLen(kMaxFrameSize);
  // Direct BLE-task callback, deliberately bypassing ada_callback heap/queue.
  ble_application_request_characteristic.setWriteCallback(
      onBleApplicationWrite, false);
  if (ble_application_request_characteristic.begin() != ERROR_NONE) return false;

  ble_application_response_characteristic.setUuid(
      BLEUuid(ble_application_response_uuid_bluefruit));
  ble_application_response_characteristic.setProperties(CHR_PROPS_INDICATE);
  ble_application_response_characteristic.setPermission(SECMODE_OPEN,
                                                        SECMODE_NO_ACCESS);
  ble_application_response_characteristic.setMaxLen(kMaxFrameSize);
  if (ble_application_response_characteristic.begin() != ERROR_NONE) return false;

  ble_application_response_value_handle =
      ble_application_response_characteristic.handles().value_handle;
  return ble_application_response_value_handle != BLE_GATT_HANDLE_INVALID;
}

void setBleApplicationIngressAllowed(bool allowed) {
  if (!ble_application_session_active) return;
  taskENTER_CRITICAL();
  ble_application_handoff.setIngressAllowed(
      ble_application_connection_handle, ble_application_session_generation,
      allowed);
  taskEXIT_CRITICAL();
}

void endBleApplicationSession() {
  if (!ble_application_session_active) return;

  // Close callback admission first, then clear the loop-owned transport. This
  // ordering prevents a late callback from seeding work during teardown.
  taskENTER_CRITICAL();
  ble_application_handoff.deactivateSession(
      ble_application_connection_handle, ble_application_session_generation);
  taskEXIT_CRITICAL();

  ble_application_transport.endSession(ble_application_session_generation);
  ble_application_session_active = false;
  ble_application_connection_handle = BLE_CONN_HANDLE_INVALID;
  ble_application_session_generation = 0;
  ble_application_indication_in_flight = false;
  ble_application_next_indication_attempt_ms = 0;
}

void beginBleApplicationSession(uint16_t connection_handle) {
  if (!ble_application_gatt_ready ||
      connection_handle == BLE_CONN_HANDLE_INVALID)
    return;

  const uint32_t generation = ble_application_transport.beginSession();
  ble_application_session_active = true;
  ble_application_connection_handle = connection_handle;
  ble_application_session_generation = generation;
  ble_application_indication_in_flight = false;
  ble_application_next_indication_attempt_ms = 0;

  taskENTER_CRITICAL();
  ble_application_handoff.activateSession(connection_handle, generation);
  taskEXIT_CRITICAL();
}

enum class BleApplicationIndicationSubmitResult : uint8_t {
  kSubmitted,
  kRetryable,
  kTerminal,
};

BleApplicationIndicationSubmitResult trySendBleApplicationIndication(
    uint16_t connection_handle, const uint8_t* frame, uint8_t frame_len,
    uint32_t& error_code) {
  uint16_t packet_len = frame_len;
  ble_gatts_hvx_params_t params{};
  params.handle = ble_application_response_value_handle;
  params.type = BLE_GATT_HVX_INDICATION;
  params.offset = 0;
  params.p_len = &packet_len;
  params.p_data = const_cast<uint8_t*>(frame);
  error_code = sd_ble_gatts_hvx(connection_handle, &params);

  if (error_code == NRF_SUCCESS) {
    if (packet_len == frame_len)
      return BleApplicationIndicationSubmitResult::kSubmitted;
    // A successful call must consume the complete bounded frame. Treat an
    // impossible partial-success result as a local terminal invariant failure
    // rather than retrying changed/ambiguous state forever.
    error_code = NRF_ERROR_DATA_SIZE;
    return BleApplicationIndicationSubmitResult::kTerminal;
  }

  // Pinned S140 6.1.1 documents these as conditions that may clear without
  // rebuilding the connection: another indication/procedure is busy, CCCD or
  // ATT-MTU state changed between the precheck and SVC, system attributes are
  // still being restored, or transient TX resources are unavailable.
  switch (error_code) {
    case NRF_ERROR_BUSY:
    case NRF_ERROR_INVALID_STATE:
    case BLE_ERROR_GATTS_SYS_ATTR_MISSING:
    case NRF_ERROR_RESOURCES:
      return BleApplicationIndicationSubmitResult::kRetryable;
    case NRF_ERROR_TIMEOUT:
      // S140 explicitly requires re-establishing the connection after this
      // result. Do not leave stop-and-wait wedged in a 25 ms retry loop.
      return BleApplicationIndicationSubmitResult::kTerminal;
    default:
      // Invalid handle/parameter/data/security/attribute errors cannot be
      // repaired by blindly retrying the same frame. Fail this application
      // session closed and recover the physical link through the same bounded
      // disconnect owner used for protocol timeout events.
      return BleApplicationIndicationSubmitResult::kTerminal;
  }
}

bool serviceBleApplicationDisconnectRecovery(
    bool connected, uint16_t connection_handle, bool disconnect_event,
    uint32_t now) {
  if (!ble_application_disconnect_pending) return false;

  // A real disconnect (including one followed by a very fast replacement
  // connection between loop polls) completes recovery. The normal session
  // logic below may then admit the current physical connection.
  if (disconnect_event || !connected ||
      connection_handle != ble_application_disconnect_handle) {
    ble_application_disconnect_pending = false;
    ble_application_disconnect_handle = BLE_CONN_HANDLE_INVALID;
    ble_application_next_disconnect_attempt_ms = 0;
    return false;
  }

  // Do not create a fresh ORUN application session on an ATT-terminal link.
  // Retry the physical disconnect at bounded spacing if the framework rejects
  // or loses the first request.
  if (orun_tlp::monotonic::reached(
          now, ble_application_next_disconnect_attempt_ms)) {
    (void)Bluefruit.disconnect(ble_application_disconnect_handle);
    ble_application_next_disconnect_attempt_ms =
        now + kBleApplicationDisconnectRetryMs;
  }
  return true;
}

void pollBleApplicationRuntime(bool connected, uint16_t connection_handle,
                               bool disconnect_event, uint32_t now) {
  if (!ble_application_gatt_ready) return;

  if (serviceBleApplicationDisconnectRecovery(
          connected, connection_handle, disconnect_event, now))
    return;

  // Disconnect cleanup happens before BleAdmissionPolicy can restart
  // advertising. A replacement session therefore cannot inherit callback
  // mailbox, transport reassembly or outbound-indication state.
  if (ble_application_session_active &&
      (disconnect_event || !connected ||
       connection_handle != ble_application_connection_handle)) {
    endBleApplicationSession();
  }

  if (connected && !ble_application_session_active) {
    beginBleApplicationSession(connection_handle);
  }
  if (!ble_application_session_active) return;

  // GATTS protocol timeout is terminal for ATT progress. Consume it before
  // HVC or ingress so no queued application work can survive the fault.
  orun_tlp::BleApplicationGattTimeoutEvent gatt_timeout;
  bool have_gatt_timeout = false;
  taskENTER_CRITICAL();
  have_gatt_timeout = ble_application_handoff.takeGattTimeout(gatt_timeout);
  taskEXIT_CRITICAL();
  if (have_gatt_timeout &&
      gatt_timeout.session_generation == ble_application_session_generation &&
      gatt_timeout.connection_handle == ble_application_connection_handle) {
    const uint16_t timed_out_handle = ble_application_connection_handle;
    endBleApplicationSession();
    ble_application_disconnect_pending = true;
    ble_application_disconnect_handle = timed_out_handle;
    ble_application_next_disconnect_attempt_ms = now;
    Serial.println(F("BLE GATT protocol timeout; disconnecting"));
    (void)serviceBleApplicationDisconnectRecovery(
        connected, connection_handle, false, now);
    return;
  }

  // Enforce the M7P7F fragment timeout before consuming a newly queued
  // continuation on this tick, so loop ordering cannot revive a stale partial.
  ble_application_transport.poll(now);

  orun_tlp::BleApplicationConfirmationEvent confirmation;
  bool have_confirmation = false;
  taskENTER_CRITICAL();
  have_confirmation =
      ble_application_handoff.takeConfirmation(confirmation);
  taskEXIT_CRITICAL();
  if (have_confirmation && ble_application_indication_in_flight &&
      confirmation.session_generation == ble_application_session_generation &&
      confirmation.connection_handle == ble_application_connection_handle &&
      confirmation.value_handle == ble_application_response_value_handle) {
    ble_application_transport.confirmOutboundFrame(
        ble_application_session_generation);
    ble_application_indication_in_flight = false;
  }

  // Reopen callback ingress only after a prior response is fully confirmed.
  setBleApplicationIngressAllowed(
      !ble_application_transport.outboundFramePending());

  orun_tlp::BleApplicationIngressEvent ingress;
  bool have_ingress = false;
  taskENTER_CRITICAL();
  have_ingress = ble_application_handoff.takeIngress(ingress);
  taskEXIT_CRITICAL();
  if (have_ingress) {
    // Snapshot only on actual application ingress, after atomically taking the
    // callback-produced frame. This avoids idle-loop work and closes the race
    // where a frame could arrive after a pre-poll "pending" sample.
    refreshApplicationStatusSnapshot(now);
    ble_application_transport.onFrameReceived(
        ingress.session_generation, ingress.frame, ingress.frame_len, now);
    setBleApplicationIngressAllowed(
        !ble_application_transport.outboundFramePending());
  }

  if (!ble_application_transport.outboundFramePending()) return;

  // Stop-and-wait is closed in callback context for the entire time the
  // response is pending, including the period before the client enables CCCD.
  setBleApplicationIngressAllowed(false);
  if (ble_application_indication_in_flight) return;

  if (!ble_application_response_characteristic.indicateEnabled(
          ble_application_connection_handle))
    return;

  if (!orun_tlp::monotonic::reached(
          now, ble_application_next_indication_attempt_ms))
    return;

  uint8_t frame[orun_tlp::ble_app_transport::kMaxFrameSize]{};
  uint8_t frame_len = 0;
  if (!ble_application_transport.peekOutboundFrame(
          ble_application_session_generation, frame, frame_len))
    return;

  // Do NOT call BLECharacteristic::indicate(): pinned Bluefruit 1.7.0 blocks
  // there waiting for HVC. Non-blocking HVX keeps the cooperative loop alive;
  // onBleEvent() hands the later HVC back for confirmOutboundFrame().
  uint32_t indication_error = NRF_SUCCESS;
  const BleApplicationIndicationSubmitResult submit_result =
      trySendBleApplicationIndication(ble_application_connection_handle, frame,
                                      frame_len, indication_error);
  if (submit_result == BleApplicationIndicationSubmitResult::kSubmitted) {
    ble_application_indication_in_flight = true;
  } else if (submit_result ==
             BleApplicationIndicationSubmitResult::kRetryable) {
    ble_application_next_indication_attempt_ms =
        now + kBleApplicationIndicationRetryMs;
  } else {
    // A terminal SVC return is equivalent to terminal ATT progress for this
    // ORUN application session. In particular, S140 documents
    // NRF_ERROR_TIMEOUT as requiring connection re-establishment. Tear down
    // application state first, then let the existing bounded loop-owned
    // disconnect recovery own the physical link.
    const uint16_t failed_handle = ble_application_connection_handle;
    endBleApplicationSession();
    ble_application_disconnect_pending = true;
    ble_application_disconnect_handle = failed_handle;
    ble_application_next_disconnect_attempt_ms = now;
    Serial.printf("BLE indication submit terminal error=0x%08lX; disconnecting\n",
                  static_cast<unsigned long>(indication_error));
    (void)serviceBleApplicationDisconnectRecovery(
        connected, connection_handle, false, now);
  }
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

void startUsbApplicationQuery(orun_tlp::ApplicationRequestKind kind) {
  refreshApplicationStatusSnapshot(orun_tlp::monotonic::nowMs());
  const orun_tlp::ApplicationRequest request(
      orun_tlp::ApplicationRequester::kUsb,
      next_usb_application_request_id, kind);
  const auto result = application_requests.submit(request);
  if (result == orun_tlp::ApplicationSubmitResult::kBusy) {
    Serial.println(F("APP BUSY"));
    return;
  }
  if (result == orun_tlp::ApplicationSubmitResult::kRejected) {
    Serial.println(F("APP REJECTED"));
    return;
  }
  ++next_usb_application_request_id;
  if (next_usb_application_request_id == 0) next_usb_application_request_id = 1;
}

void drainApplicationResponse() {
  orun_tlp::ApplicationResponse response;
  if (!application_requests.takeResponse(
          orun_tlp::ApplicationRequester::kUsb, response))
    return;
  orun_tlp::printUsbApplicationResponse(response);
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

#ifdef ORUN_M7P6E_CRYPTO_BLE_PROBE
using M7P6EBleSecurityCounters =
    orun_tlp::m7p6e_test::PairingEvidenceCounters;

M7P6EBleSecurityCounters readM7P6EBleSecurityCounters() {
  M7P6EBleSecurityCounters out;
  taskENTER_CRITICAL();
  out.lesc = m7p6e_lesc_dhkey_events;
  out.auth = m7p6e_auth_status_events;
  out.auth_success = m7p6e_auth_success_events;
  out.auth_failure = m7p6e_auth_failure_events;
  out.auth_bonded_success = m7p6e_auth_bonded_success_events;
  out.auth_lesc_bonded_success = m7p6e_auth_lesc_bonded_success_events;
  out.sec_update = m7p6e_sec_update_events;
  out.encrypted_update = m7p6e_encrypted_sec_update_events;
  out.disconnects = ble_disconnect_events;
  taskEXIT_CRITICAL();
  return out;
}

void printM7P6EKat(const char* prefix,
                   const orun_tlp::m7p6e_test::KatResult& r) {
  Serial.printf(
      "%s %s hkdf_d2a=%s hkdf_a2d=%s dir_sep=%s nonce=%s "
      "ccm_encrypt=%s ccm_decrypt=%s tamper=%s recovery=%s "
      "tamper_rc=0x%08lX\n",
      prefix, r.pass ? "PASS" : "FAIL",
      r.hkdf_d2a ? "PASS" : "FAIL",
      r.hkdf_a2d ? "PASS" : "FAIL",
      r.direction_separated ? "PASS" : "FAIL",
      r.nonce_layout ? "PASS" : "FAIL",
      r.ccm_encrypt ? "PASS" : "FAIL",
      r.ccm_decrypt ? "PASS" : "FAIL",
      r.tamper_rejected ? "PASS" : "FAIL",
      r.recovery ? "PASS" : "FAIL",
      static_cast<unsigned long>(r.tamper_result));
}

void printM7P6ECryptoStatus() {
  const M7P6EBleSecurityCounters counters = readM7P6EBleSecurityCounters();
  Serial.printf(
      "M7P6E STATUS boot_kat=%s stress=%s iterations=%u "
      "lesc_events=%lu auth_events=%lu auth_success=%lu auth_failure=%lu "
      "bonded_success=%lu lesc_bonded_success=%lu "
      "sec_update_events=%lu encrypted_updates=%lu "
      "ble_connected=%u\n",
      m7p6e_boot_kat_pass ? "PASS" : "FAIL",
      m7p6e_crypto_stress.active ? "ACTIVE" : "IDLE",
      static_cast<unsigned>(m7p6e_crypto_stress.iterations),
      static_cast<unsigned long>(counters.lesc),
      static_cast<unsigned long>(counters.auth),
      static_cast<unsigned long>(counters.auth_success),
      static_cast<unsigned long>(counters.auth_failure),
      static_cast<unsigned long>(counters.auth_bonded_success),
      static_cast<unsigned long>(counters.auth_lesc_bonded_success),
      static_cast<unsigned long>(counters.sec_update),
      static_cast<unsigned long>(counters.encrypted_update),
      ble_ready ? static_cast<unsigned>(Bluefruit.Periph.connected()) : 0U);
}

void runM7P6ECryptoProbe() {
  if (!ble_ready) {
    Serial.println(F("M7P6E KAT REJECT reason=ble-not-ready"));
    return;
  }
  const auto result = orun_tlp::m7p6e_test::runCandidateKat();
  printM7P6EKat("M7P6E KAT", result);
}

void startM7P6ECryptoStress() {
  if (!ble_ready) {
    Serial.println(F("M7P6E COEX REJECT reason=ble-not-ready"));
    return;
  }
  if (!m7p6e_boot_kat_pass) {
    Serial.println(F("M7P6E COEX REJECT reason=boot-kat-failed"));
    return;
  }
  if (m7p6e_crypto_stress.active) {
    Serial.println(F("M7P6E COEX REJECT reason=already-active"));
    return;
  }
  const unsigned connected =
      static_cast<unsigned>(Bluefruit.Periph.connected());
  if (connected != 1U) {
    Serial.printf("M7P6E COEX REJECT reason=ble-clients-%u expected=1\n",
                  connected);
    return;
  }

  const M7P6EBleSecurityCounters counters = readM7P6EBleSecurityCounters();
  m7p6e_crypto_stress = M7P6ECryptoStressState{};
  m7p6e_crypto_stress.active = true;
  m7p6e_crypto_stress.next_iteration_ms = orun_tlp::monotonic::nowMs();
  m7p6e_crypto_stress.evidence_start = counters;
  Serial.println(
      F("M7P6E COEX START iterations_max=3000 spacing_ms=20 "
        "action=trigger-BLE-bond-now"));
}

void pollM7P6ECryptoStress() {
  if (!m7p6e_crypto_stress.active) return;

  const M7P6EBleSecurityCounters& start =
      m7p6e_crypto_stress.evidence_start;
  M7P6EBleSecurityCounters counters = readM7P6EBleSecurityCounters();
  unsigned connected =
      ble_ready ? static_cast<unsigned>(Bluefruit.Periph.connected()) : 0U;

  if (connected != 1U ||
      orun_tlp::m7p6e_test::pairingEvidenceDisconnected(start, counters)) {
    m7p6e_crypto_stress.active = false;
    Serial.printf(
        "M7P6E COEX FAIL reason=ble-lost iterations=%u connected=%u "
        "disconnect_delta=%lu\n",
        static_cast<unsigned>(m7p6e_crypto_stress.iterations), connected,
        static_cast<unsigned long>(counters.disconnects - start.disconnects));
    return;
  }

  if (orun_tlp::m7p6e_test::pairingEvidenceRejected(start, counters)) {
    m7p6e_crypto_stress.active = false;
    Serial.printf(
        "M7P6E COEX FAIL reason=pairing-auth iterations=%u "
        "auth_failure_delta=%lu ble_connected=1\n",
        static_cast<unsigned>(m7p6e_crypto_stress.iterations),
        static_cast<unsigned long>(
            counters.auth_failure - start.auth_failure));
    return;
  }

  const uint32_t now = orun_tlp::monotonic::nowMs();
  if (!orun_tlp::monotonic::reached(
          now, m7p6e_crypto_stress.next_iteration_ms)) {
    return;
  }
  m7p6e_crypto_stress.next_iteration_ms = now + 20U;

  const uint32_t started_us = micros();
  const auto result = orun_tlp::m7p6e_test::runCandidateKat();
  const uint32_t elapsed_us = static_cast<uint32_t>(micros() - started_us);
  if (elapsed_us > m7p6e_crypto_stress.max_kat_us)
    m7p6e_crypto_stress.max_kat_us = elapsed_us;

  ++m7p6e_crypto_stress.iterations;
  if (!result.pass) {
    m7p6e_crypto_stress.active = false;
    printM7P6EKat("M7P6E COEX KAT", result);
    Serial.printf("M7P6E COEX FAIL reason=kat iteration=%u\n",
                  static_cast<unsigned>(m7p6e_crypto_stress.iterations));
    return;
  }

  // Re-sample after the KAT because BLE security events may arrive while CC310
  // work is running. This prevents a last-iteration pairing event from being
  // missed and, more importantly, catches a rejection/disconnect that occurred
  // during the KAT before any PASS decision is made.
  counters = readM7P6EBleSecurityCounters();
  connected =
      ble_ready ? static_cast<unsigned>(Bluefruit.Periph.connected()) : 0U;
  if (connected != 1U ||
      orun_tlp::m7p6e_test::pairingEvidenceDisconnected(start, counters)) {
    m7p6e_crypto_stress.active = false;
    Serial.printf(
        "M7P6E COEX FAIL reason=ble-lost iterations=%u connected=%u "
        "disconnect_delta=%lu\n",
        static_cast<unsigned>(m7p6e_crypto_stress.iterations), connected,
        static_cast<unsigned long>(counters.disconnects - start.disconnects));
    return;
  }
  if (orun_tlp::m7p6e_test::pairingEvidenceRejected(start, counters)) {
    m7p6e_crypto_stress.active = false;
    Serial.printf(
        "M7P6E COEX FAIL reason=pairing-auth iterations=%u "
        "auth_failure_delta=%lu ble_connected=1\n",
        static_cast<unsigned>(m7p6e_crypto_stress.iterations),
        static_cast<unsigned long>(
            counters.auth_failure - start.auth_failure));
    return;
  }

  if (!m7p6e_crypto_stress.lesc_overlap_seen &&
      orun_tlp::m7p6e_test::counterAdvanced(start.lesc, counters.lesc)) {
    m7p6e_crypto_stress.lesc_overlap_seen = true;
    m7p6e_crypto_stress.lesc_seen_iteration =
        m7p6e_crypto_stress.iterations;
    Serial.printf("M7P6E LESC OVERLAP observed iteration=%u\n",
                  static_cast<unsigned>(
                      m7p6e_crypto_stress.lesc_seen_iteration));
  }

  if (!m7p6e_crypto_stress.pairing_complete_seen &&
      orun_tlp::m7p6e_test::pairingEvidenceComplete(start, counters)) {
    m7p6e_crypto_stress.pairing_complete_seen = true;
    m7p6e_crypto_stress.pairing_complete_iteration =
        m7p6e_crypto_stress.iterations;
    Serial.printf(
        "M7P6E PAIRING COMPLETE iteration=%u auth_success_delta=%lu "
        "bonded_success_delta=%lu lesc_bonded_success_delta=%lu "
        "encrypted_update_delta=%lu\n",
        static_cast<unsigned>(
            m7p6e_crypto_stress.pairing_complete_iteration),
        static_cast<unsigned long>(
            counters.auth_success - start.auth_success),
        static_cast<unsigned long>(
            counters.auth_bonded_success - start.auth_bonded_success),
        static_cast<unsigned long>(
            counters.auth_lesc_bonded_success -
            start.auth_lesc_bonded_success),
        static_cast<unsigned long>(
            counters.encrypted_update - start.encrypted_update));
  }

  constexpr uint16_t kPostPairingIterations = 100U;
  if (m7p6e_crypto_stress.pairing_complete_seen &&
      static_cast<uint16_t>(
          m7p6e_crypto_stress.iterations -
          m7p6e_crypto_stress.pairing_complete_iteration) >=
          kPostPairingIterations) {
    // Use the post-KAT snapshot that was just checked for disconnect/auth
    // failure above. Re-reading here could capture a failure/disconnect that
    // arrived after the check yet still print PASS with non-zero failure
    // deltas, which would make the evidence self-contradictory.
    m7p6e_crypto_stress.active = false;
    Serial.printf(
        "M7P6E COEX PASS iterations=%u lesc_delta=%lu auth_delta=%lu "
        "auth_success_delta=%lu bonded_success_delta=%lu "
        "lesc_bonded_success_delta=%lu auth_failure_delta=%lu "
        "sec_update_delta=%lu "
        "encrypted_update_delta=%lu disconnect_delta=%lu max_kat_us=%lu "
        "ble_connected=1\n",
        static_cast<unsigned>(m7p6e_crypto_stress.iterations),
        static_cast<unsigned long>(counters.lesc - start.lesc),
        static_cast<unsigned long>(counters.auth - start.auth),
        static_cast<unsigned long>(
            counters.auth_success - start.auth_success),
        static_cast<unsigned long>(
            counters.auth_bonded_success -
            start.auth_bonded_success),
        static_cast<unsigned long>(
            counters.auth_lesc_bonded_success -
            start.auth_lesc_bonded_success),
        static_cast<unsigned long>(
            counters.auth_failure - start.auth_failure),
        static_cast<unsigned long>(
            counters.sec_update - start.sec_update),
        static_cast<unsigned long>(
            counters.encrypted_update - start.encrypted_update),
        static_cast<unsigned long>(
            counters.disconnects - start.disconnects),
        static_cast<unsigned long>(m7p6e_crypto_stress.max_kat_us));
    return;
  }

  constexpr uint16_t kMaxIterations = 3000U;
  if (m7p6e_crypto_stress.iterations >= kMaxIterations) {
    m7p6e_crypto_stress.active = false;
    const char* reason =
        !m7p6e_crypto_stress.lesc_overlap_seen
            ? "no-lesc-overlap"
            : !m7p6e_crypto_stress.pairing_complete_seen
                  ? "pairing-not-complete"
                  : "post-pairing-window-incomplete";
    Serial.printf(
        "M7P6E COEX FAIL reason=%s iterations=%u lesc_delta=%lu "
        "auth_delta=%lu auth_success_delta=%lu bonded_success_delta=%lu "
        "lesc_bonded_success_delta=%lu auth_failure_delta=%lu "
        "sec_update_delta=%lu "
        "encrypted_update_delta=%lu max_kat_us=%lu\n",
        reason,
        static_cast<unsigned>(m7p6e_crypto_stress.iterations),
        static_cast<unsigned long>(counters.lesc - start.lesc),
        static_cast<unsigned long>(counters.auth - start.auth),
        static_cast<unsigned long>(
            counters.auth_success - start.auth_success),
        static_cast<unsigned long>(
            counters.auth_bonded_success - start.auth_bonded_success),
        static_cast<unsigned long>(
            counters.auth_lesc_bonded_success -
            start.auth_lesc_bonded_success),
        static_cast<unsigned long>(
            counters.auth_failure - start.auth_failure),
        static_cast<unsigned long>(
            counters.sec_update - start.sec_update),
        static_cast<unsigned long>(
            counters.encrypted_update - start.encrypted_update),
        static_cast<unsigned long>(m7p6e_crypto_stress.max_kat_us));
  }
}
#endif

#ifdef ORUN_M7P7B_FLASH_PROBE
// M7P7B TEMPORARY test-only physical probe (see m7p7b_flash_probe.h). Exists
// only in the rak4630_m7p7b_flash_probe env; never in production.
orun_tlp::ConfigFlashProbe flash_probe;

const char* configFlashProbeEvidenceName(
    orun_tlp::config_format::PageEvidence evidence) {
  using E = orun_tlp::config_format::PageEvidence;
  switch (evidence) {
    case E::kErased: return "ERASED";
    case E::kLegacyV1Committed: return "LEGACY_V1_COMMITTED";
    case E::kLegacyV1UncommittedOrTorn: return "LEGACY_V1_TORN";
    case E::kLegacyV1CommittedCorrupt: return "LEGACY_V1_CORRUPT";
    case E::kV2Staged: return "V2_STAGED";
    case E::kV2UncommittedOrTorn: return "V2_TORN";
    case E::kV2PartialCommit: return "V2_PARTIAL_COMMIT";
    case E::kV2Committed: return "V2_COMMITTED";
    case E::kV2CommittedRetired: return "V2_COMMITTED_RETIRED";
    case E::kV2CommittedCorrupt: return "V2_COMMITTED_CORRUPT";
    case E::kSupportedCorrupt: return "SUPPORTED_CORRUPT";
    case E::kUnsupportedNewer: return "UNSUPPORTED_NEWER";
  }
  return "UNKNOWN";
}

void printConfigFlashProbeU64(uint64_t value) {
  Serial.printf("%08lX%08lX",
                static_cast<unsigned long>(
                    static_cast<uint32_t>(value >> 32)),
                static_cast<unsigned long>(static_cast<uint32_t>(value)));
}

void printConfigFlashProbeLineage(const char* phase) {
  orun_tlp::config_format::StateToken token;
  Serial.printf("FLASH PROBE LINEAGE phase=%s token_state=%s",
                phase,
                config_store.stateToken(token) ? "VALID" : "UNAVAILABLE");
  if (config_store.stateToken(token)) {
    Serial.print(" incarnation=0x");
    printConfigFlashProbeU64(token.incarnation);
    Serial.printf(" revision=%lu",
                  static_cast<unsigned long>(token.revision));
  }
  Serial.println();

  for (uint32_t page = 0; page < 2U; ++page) {
    uint8_t prefix[orun_tlp::config_format::kV2PagePrefixSize]{};
    orun_tlp::config_format::PageInspection inspection;
    const bool read_ok = storage_flash_gate.configPort().read(
        page * orun_tlp::storage_config::kPageSize,
        prefix, sizeof(prefix));
    const bool inspect_ok =
        read_ok &&
        orun_tlp::config_format::inspectPagePrefix(
            prefix, sizeof(prefix), inspection);
    if (!inspect_ok) {
      Serial.printf("FLASH PROBE LINEAGE phase=%s page=%c read=FAIL\n",
                    phase, page == 0U ? 'A' : 'B');
      continue;
    }

    Serial.printf("FLASH PROBE LINEAGE phase=%s page=%c evidence=%s decoded=%s",
                  phase, page == 0U ? 'A' : 'B',
                  configFlashProbeEvidenceName(inspection.evidence),
                  inspection.has_decoded_record ? "yes" : "no");
    if (inspection.has_decoded_record) {
      Serial.print(" generation=0x");
      printConfigFlashProbeU64(inspection.generation);
      Serial.print(" incarnation=0x");
      printConfigFlashProbeU64(inspection.token.incarnation);
      Serial.printf(
          " revision=%lu tracking_interval_seconds=%lu battery_capacity_mah=%lu",
          static_cast<unsigned long>(inspection.token.revision),
          static_cast<unsigned long>(
              inspection.config.tracking_interval_seconds),
          static_cast<unsigned long>(
              inspection.config.battery_capacity_mah));
    }
    Serial.println();
  }
  Serial.flush();
}

orun_tlp::ConfigFlashProbe::Inputs flashProbeInputs() {
  orun_tlp::ConfigFlashProbe::Inputs in;
  in.now_ms = orun_tlp::monotonic::nowMs();
  in.ble_connected = ble_ready ? Bluefruit.Periph.connected() : 0;
  taskENTER_CRITICAL();
  in.ble_disconnect_events = ble_disconnect_events;
  taskEXIT_CRITICAL();
  const auto& d = storage_flash_gate.configDiagnostics();
  in.async.async_accepted = d.async_accepted;
  in.async.completions_success = d.completions_success;
  in.async.completions_error = d.completions_error;
  in.async.timeouts = d.timeouts;
  in.async.late_completions = d.late_completions;
  return in;
}

void startFlashProbe() {
  using Probe = orun_tlp::ConfigFlashProbe;
  const Probe::Inputs in = flashProbeInputs();
  const auto result = flash_probe.start(config_store, ble_ready, in);
  const char* reason = nullptr;
  switch (result) {
    case Probe::StartResult::kStarted:
      Serial.printf(
          "FLASH PROBE START orig_interval=%lu orig_mah=%lu temp_mah=%lu "
          "ble_connected=1 disconnect_snapshot=%lu async_accepted=%lu "
          "stale_result=%s\n",
          static_cast<unsigned long>(
              flash_probe.report().original.tracking_interval_seconds),
          static_cast<unsigned long>(flash_probe.report().original.battery_capacity_mah),
          static_cast<unsigned long>(flash_probe.temporaryConfig().battery_capacity_mah),
          static_cast<unsigned long>(in.ble_disconnect_events),
          static_cast<unsigned long>(in.async.async_accepted),
          flash_probe.staleResultDrained() ? "drained" : "none");
      printConfigFlashProbeLineage("start");
      return;
    case Probe::StartResult::kNotIdle: reason = "not-idle"; break;
    case Probe::StartResult::kConfigNotReady: reason = "config-not-ready-or-busy"; break;
    case Probe::StartResult::kBleNotReady: reason = "ble-not-ready"; break;
    case Probe::StartResult::kBleClientCount: reason = "ble-clients-not-1"; break;
    case Probe::StartResult::kTempRejected: reason = "temp-save-rejected"; break;
  }
  Serial.printf("FLASH PROBE REJECT reason=%s ble_connected=%u config unchanged\n",
                reason, static_cast<unsigned>(in.ble_connected));
}

void printFlashProbeReport() {
  using Probe = orun_tlp::ConfigFlashProbe;
  const Probe::Report& r = flash_probe.report();
  const auto ul = [](uint32_t v) { return static_cast<unsigned long>(v); };
  printConfigFlashProbeLineage("final");
  if (r.pass) {
    Serial.printf(
        "FLASH PROBE PASS temp_verified=yes restore_verified=yes ble_connected=yes "
        "ble_disconnects=%lu async_accepted_delta=%lu completions_success_delta=%lu "
        "errors_delta=%lu timeouts_delta=%lu late_delta=%lu\n",
        ul(r.ble_disconnects), ul(r.accepted_delta), ul(r.success_delta),
        ul(r.errors_delta), ul(r.timeouts_delta), ul(r.late_delta));
    return;
  }
  const char* stage = "unknown";
  switch (r.failure) {
    case Probe::Failure::kTempSave: stage = "temp-save"; break;
    case Probe::Failure::kTempVerify: stage = "temp-verify"; break;
    case Probe::Failure::kRestoreRequest: stage = "restore-request"; break;
    case Probe::Failure::kRestoreSave: stage = "restore-save"; break;
    case Probe::Failure::kRestoreVerify: stage = "restore-verify"; break;
    case Probe::Failure::kTimeout: stage = "timeout"; break;
    case Probe::Failure::kBleLost: stage = "ble"; break;
    case Probe::Failure::kAsyncEvidence: stage = "async-evidence"; break;
    case Probe::Failure::kNone: break;
  }
  Serial.printf(
      "FLASH PROBE FAIL%s stage=%s temp_verified=%s restore_verified=%s "
      "ble_connected=%s ble_disconnects=%lu async_accepted_delta=%lu "
      "completions_success_delta=%lu errors_delta=%lu timeouts_delta=%lu "
      "late_delta=%lu\n",
      r.restore_failure ? " restore" : "", stage, r.temp_verified ? "yes" : "no",
      r.restore_verified ? "yes" : "no", r.ble_connected ? "yes" : "no",
      ul(r.ble_disconnects), ul(r.accepted_delta), ul(r.success_delta),
      ul(r.errors_delta), ul(r.timeouts_delta), ul(r.late_delta));
  if (r.restore_failure) {
    Serial.printf(
        "FLASH PROBE FAIL restore config: current_interval=%lu current_mah=%lu "
        "original_interval=%lu original_mah=%lu\n",
        ul(r.current.tracking_interval_seconds), ul(r.current.battery_capacity_mah),
        ul(r.original.tracking_interval_seconds), ul(r.original.battery_capacity_mah));
  }
}
#endif

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
  orun_tlp::ApplicationRequestKind application_query_kind;
  if (orun_tlp::parseUsbApplicationQuery(
          role_command, role_command_length, &application_query_kind)) {
    role_command_length = 0;
    startUsbApplicationQuery(application_query_kind);
    return;
  }
#ifdef ORUN_M7P6E_CRYPTO_BLE_PROBE
  if (isActivityCommand("CRYPTO?", 7)) {
    role_command_length = 0;
    printM7P6ECryptoStatus();
    return;
  }
  if (isActivityCommand("CRYPTO PROBE", 12)) {
    role_command_length = 0;
    runM7P6ECryptoProbe();
    return;
  }
  if (isActivityCommand("CRYPTO STRESS", 13)) {
    role_command_length = 0;
    startM7P6ECryptoStress();
    return;
  }
#endif
#ifdef ORUN_M7P7B_FLASH_PROBE
  if (isActivityCommand("FLASH PROBE", 11)) {
    role_command_length = 0;
    startFlashProbe();
    return;
  }
#endif
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


void refreshApplicationStatusSnapshot(uint32_t now_ms) {
  const orun_tlp::CapabilitySnapshot capabilities = currentCapabilitySnapshot();
  const orun_tlp::RequestedConfig requested =
      orun_tlp::requestedConfigFromLegacyBehavior(
          orun_tlp::legacyRoleBehavior(role_controller.role()));
  const orun_tlp::EffectiveConfig effective =
      orun_tlp::resolveRequestedConfig(requested, capabilities);
  orun_tlp::buildApplicationStatusSnapshot(
      now_ms,
      role_controller.role(),
      role_controller.automatic(),
      capabilities,
      effective,
      radio_manager.relayForwardingEnabled(),
      active_tracking_base_interval_seconds,
      gnss_manager,
      config_store,
      geofence_store,
      geofence_confirmation,
      history,
      security_store,
      application_status_snapshot);
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

#ifdef ORUN_M6D2_GEOFENCE_PROBE
const char* m6d2ProbeRelationName(orun_tlp::PermittedAreaRelation relation) {
  switch (relation) {
    case orun_tlp::PermittedAreaRelation::kInside: return "INSIDE";
    case orun_tlp::PermittedAreaRelation::kBoundary: return "BOUNDARY";
    case orun_tlp::PermittedAreaRelation::kOutside: return "OUTSIDE";
    case orun_tlp::PermittedAreaRelation::kInvalidAreaSet: return "INVALID_SET";
    case orun_tlp::PermittedAreaRelation::kInvalidPoint: return "INVALID_POINT";
  }
  return "UNKNOWN";
}

const char* m6d2ProbeOperationalName(
    orun_tlp::GeofenceOperationalResult result) {
  switch (result) {
    case orun_tlp::GeofenceOperationalResult::kInvalidObservation:
      return "INVALID_OBSERVATION";
    case orun_tlp::GeofenceOperationalResult::kBoundaryNoDecision:
      return "BOUNDARY_NO_DECISION";
    case orun_tlp::GeofenceOperationalResult::kInitializedInside:
      return "INITIALIZED_INSIDE";
    case orun_tlp::GeofenceOperationalResult::kInitializedOutside:
      return "INITIALIZED_OUTSIDE";
    case orun_tlp::GeofenceOperationalResult::kStableInside:
      return "STABLE_INSIDE";
    case orun_tlp::GeofenceOperationalResult::kStableOutside:
      return "STABLE_OUTSIDE";
    case orun_tlp::GeofenceOperationalResult::kConfirmationStarted:
      return "CONFIRMATION_STARTED";
    case orun_tlp::GeofenceOperationalResult::kConfirmationContinues:
      return "CONFIRMATION_CONTINUES";
    case orun_tlp::GeofenceOperationalResult::kConfirmationRejected:
      return "CONFIRMATION_REJECTED";
    case orun_tlp::GeofenceOperationalResult::kConfirmedOutside:
      return "CONFIRMED_OUTSIDE";
    case orun_tlp::GeofenceOperationalResult::kConfirmedInside:
      return "CONFIRMED_INSIDE";
  }
  return "UNKNOWN";
}

bool configureM6D2ProbeAround(const orun_tlp::GnssFix& fix) {
  if (m6d2_probe_configured) return true;

  const int64_t min_lat =
      static_cast<int64_t>(fix.latitude_e7) - kM6D2ProbeHalfSpanE7;
  const int64_t max_lat =
      static_cast<int64_t>(fix.latitude_e7) + kM6D2ProbeHalfSpanE7;
  const int64_t min_lon =
      static_cast<int64_t>(fix.longitude_e7) - kM6D2ProbeHalfSpanE7;
  const int64_t max_lon =
      static_cast<int64_t>(fix.longitude_e7) + kM6D2ProbeHalfSpanE7;
  if (min_lat <= -900000000LL || max_lat >= 900000000LL ||
      min_lon <= -1800000000LL || max_lon >= 1800000000LL) {
    Serial.println(F("M6D2 PROBE fixture rejected near global singularity"));
    return false;
  }

  m6d2_probe_vertices[0] =
      orun_tlp::GeoPointE7(static_cast<int32_t>(min_lat),
                           static_cast<int32_t>(min_lon));
  m6d2_probe_vertices[1] =
      orun_tlp::GeoPointE7(static_cast<int32_t>(min_lat),
                           static_cast<int32_t>(max_lon));
  m6d2_probe_vertices[2] =
      orun_tlp::GeoPointE7(static_cast<int32_t>(max_lat),
                           static_cast<int32_t>(max_lon));
  m6d2_probe_vertices[3] =
      orun_tlp::GeoPointE7(static_cast<int32_t>(max_lat),
                           static_cast<int32_t>(min_lon));
  m6d2_probe_areas[0] =
      orun_tlp::GeofencePolygonView(m6d2_probe_vertices, 4);

  if (geofence_confirmation.configure(
          orun_tlp::GeofenceAreaSetView(m6d2_probe_areas, 1)) !=
      orun_tlp::GeofenceRuntimeConfigResult::kApplied) {
    Serial.println(F("M6D2 PROBE fixture configure failed"));
    return false;
  }

  m6d2_probe_configured = true;
  Serial.printf(
      "M6D2 PROBE ARMED center_lat=%ld center_lon=%ld halfspan_e7=%ld "
      "base_s=%lu\n",
      static_cast<long>(fix.latitude_e7), static_cast<long>(fix.longitude_e7),
      static_cast<long>(kM6D2ProbeHalfSpanE7),
      static_cast<unsigned long>(active_tracking_base_interval_seconds));
  Serial.println(F(
      "M6D2 PROBE NOTE GNSS FIX ttff is total acquisition age, not inter-fix latency"));
  return true;
}
#endif

void clearGeofenceEpisodeFixes() {
  for (uint8_t i = 0;
       i < orun_tlp::geofence_operational_config::kConfirmationObservationLimit;
       ++i) {
    geofence_episode_fixes[i] = orun_tlp::GnssFix{};
  }
  geofence_slot0_normal_store_expected = false;
  geofence_slot0_normal_store_accepted = false;
}

void abortGeofenceConfirmation(const char* reason) {
  if (!geofence_confirmation.abortConfirmation()) return;
  (void)gnss_manager.cancelAdditionalFixAcquisition();
  clearGeofenceEpisodeFixes();
  Serial.printf("GEOFENCE confirmation aborted reason=%s\n", reason);
}

const char* geofenceTokenStateName(orun_tlp::GeofenceTokenState state) {
  switch (state) {
    case orun_tlp::GeofenceTokenState::kUnavailable: return "UNAVAILABLE";
    case orun_tlp::GeofenceTokenState::kValid: return "VALID";
    case orun_tlp::GeofenceTokenState::kUncertain: return "UNCERTAIN";
  }
  return "UNKNOWN";
}

void applyGeofenceCadence(
    orun_tlp::GeofenceCadenceMode cadence_mode) {
  const uint32_t effective_interval_ms =
      orun_tlp::geofence_runtime_policy::effectiveTrackingIntervalMs(
          active_tracking_base_interval_seconds, cadence_mode);
  if (effective_interval_ms == 0 ||
      !gnss_manager.setTrackingIntervalMsAndReanchor(effective_interval_ms)) {
    // Do not overwrite durable B or guess a schedule. This should be reachable
    // only if composition ordering is broken because the fresh fix has already
    // been consumed but GnssManager must still be at kFixAvailable.
    Serial.println(F("GEOFENCE cadence re-anchor rejected"));
  } else {
#ifdef ORUN_M6D2_GEOFENCE_PROBE
    Serial.printf("M6D2 PROBE CADENCE interval_ms=%lu gnss_policy=%s\n",
                  static_cast<unsigned long>(effective_interval_ms),
                  effective_interval_ms <=
                          orun_tlp::gnss_config::kShortIntervalThresholdMs
                      ? "KEEP_POWERED"
                      : "POWER_CYCLE_ELIGIBLE");
#endif
  }
}

void processGeofenceAcceptedFix(const orun_tlp::GnssFix& fix,
                                bool ordinary_position_will_follow) {
#ifdef ORUN_M6D2_GEOFENCE_PROBE
  if (!geofence_confirmation.configured() && !configureM6D2ProbeAround(fix)) {
    return;
  }
#endif
  if (!geofence_confirmation.configured()) return;

  const auto update = geofence_confirmation.observeAcceptedLocation(
      orun_tlp::GeoPointE7(fix.latitude_e7, fix.longitude_e7),
      fix.captured_at_ms, fix.hdop_x100, fix.satellites);

#ifdef ORUN_M6D2_GEOFENCE_PROBE
  if (update.geometry_result == orun_tlp::GeofenceObservationResult::kAccepted) {
    const orun_tlp::PermittedAreaAssessment probe_assessment =
        orun_tlp::assessPermittedGeofenceAreas(
            orun_tlp::GeofenceAreaSetView(m6d2_probe_areas, 1),
            orun_tlp::GeoPointE7(fix.latitude_e7, fix.longitude_e7));
    Serial.printf(
        "M6D2 PROBE OBS relation=%s op=%s slot=%u request_more=%s "
        "cadence=%s lat=%ld lon=%ld hdop=%u.%02u sats=%u\n",
        m6d2ProbeRelationName(probe_assessment.relation),
        m6d2ProbeOperationalName(update.operational_result),
        static_cast<unsigned>(update.episode_slot),
        update.request_additional_observation ? "yes" : "no",
        update.cadence_mode == orun_tlp::GeofenceCadenceMode::kBase
            ? "BASE"
            : "BASE/3",
        static_cast<long>(fix.latitude_e7), static_cast<long>(fix.longitude_e7),
        fix.hdop_x100 / 100, fix.hdop_x100 % 100, fix.satellites);
  }
#endif

  if (update.confirmation_timed_out) {
    (void)gnss_manager.cancelAdditionalFixAcquisition();
    clearGeofenceEpisodeFixes();
    Serial.println(F("GEOFENCE confirmation timeout"));
    return;
  }

  if (update.geometry_result != orun_tlp::GeofenceObservationResult::kAccepted) {
    if (update.request_additional_observation) {
      if (!gnss_manager.continueCurrentAcquisitionForAdditionalFix()) {
        abortGeofenceConfirmation("GNSS_CONTINUATION_REJECTED");
      }
    } else if (geofence_confirmation.confirmationActive()) {
      abortGeofenceConfirmation("GEOFENCE_GEOMETRY_FAULT");
    }
    return;
  }

  if (update.episode_evidence_accepted &&
      update.episode_slot <
          orun_tlp::geofence_operational_config::kConfirmationObservationLimit) {
    geofence_episode_fixes[update.episode_slot] = fix;
    if (update.episode_slot == 0 && ordinary_position_will_follow) {
      geofence_slot0_normal_store_expected = true;
      geofence_slot0_normal_store_accepted = false;
    }
  }

  if (update.cadence_changed) applyGeofenceCadence(update.cadence_mode);

  if (update.representative_available &&
      update.representative_slot <
          orun_tlp::geofence_operational_config::kConfirmationObservationLimit) {
    // Slot 0 already entered the ordinary store-before-send path when it
    // started the episode. Reusing that exact observation as representative
    // must not create a second sequence/history/TX record.
    if (!(update.representative_slot == 0 &&
          geofence_slot0_normal_store_accepted)) {
      geofence_representative_fix =
          geofence_episode_fixes[update.representative_slot];
      geofence_representative_pending = true;
    }
  }

  if (update.outside_event_occurrence) {
    // Local semantic occurrence only. Current TLP v1 has no authorized secure
    // EVENT packet; do not turn this into an unauthenticated remote alarm.
    Serial.println(F("GEOFENCE OUTSIDE confirmed (local event occurrence)"));
  }

  if (update.request_additional_observation) {
    if (!gnss_manager.continueCurrentAcquisitionForAdditionalFix()) {
      abortGeofenceConfirmation("GNSS_CONTINUATION_REJECTED");
    }
  } else if (!geofence_confirmation.confirmationActive()) {
    clearGeofenceEpisodeFixes();
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

#ifdef ORUN_M7P6I_HISTORY_CRYPTO_PROBE
orun_tlp::m7p6i_test::HistoryCryptoKatResult m7p6i_history_crypto_kat{};
bool m7p6i_history_crypto_kat_done = false;
uint32_t m7p6i_history_crypto_last_report_ms = 0;

void printM7P6IHistoryCryptoKat() {
  Serial.printf(
      "M7P6I HISTORY CRYPTO KAT %s provision=%s observation=%s "
      "receipt=%s tamper=%s recovery=%s\n",
      m7p6i_history_crypto_kat.pass() ? "PASS" : "FAIL",
      m7p6i_history_crypto_kat.provision ? "PASS" : "FAIL",
      m7p6i_history_crypto_kat.observation ? "PASS" : "FAIL",
      m7p6i_history_crypto_kat.receipt ? "PASS" : "FAIL",
      m7p6i_history_crypto_kat.tamper_rejected ? "PASS" : "FAIL",
      m7p6i_history_crypto_kat.recovery ? "PASS" : "FAIL");
  m7p6i_history_crypto_last_report_ms = millis();
}
#endif

#ifdef ORUN_M4P5C_HISTORY_RECEIPT_PROBE
orun_tlp::m4p5c_test::HistoryReceiptKatResult m4p5c_history_receipt_kat{};
bool m4p5c_history_receipt_kat_done = false;
uint32_t m4p5c_history_receipt_last_report_ms = 0;

void printM4P5CHistoryReceiptKat() {
  Serial.printf(
      "M4P5C HISTORY RECEIPT KAT %s history=%s provision=%s opaque=%s "
      "busy=%s replaywait=%s applied=%s tamper=%s duplicate=%s\n",
      m4p5c_history_receipt_kat.pass() ? "PASS" : "FAIL",
      m4p5c_history_receipt_kat.history_ready ? "PASS" : "FAIL",
      m4p5c_history_receipt_kat.provision ? "PASS" : "FAIL",
      m4p5c_history_receipt_kat.opaque_started ? "PASS" : "FAIL",
      m4p5c_history_receipt_kat.busy_guard ? "PASS" : "FAIL",
      m4p5c_history_receipt_kat.replay_wait ? "PASS" : "FAIL",
      m4p5c_history_receipt_kat.applied ? "PASS" : "FAIL",
      m4p5c_history_receipt_kat.tamper_rejected ? "PASS" : "FAIL",
      m4p5c_history_receipt_kat.duplicate_rejected ? "PASS" : "FAIL");
  m4p5c_history_receipt_last_report_ms = millis();
}
#endif

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
  } else if (history.formatResetRequired()) {
    Serial.println(F("STORAGE v3 development reset required; POSITION TX disabled"));
  } else {
    Serial.println(F("STORAGE unavailable; POSITION TX disabled"));
  }
  // M7P5: recover durable config before GNSS starts, so the very first
  // acquisition schedule already reflects it. config_store.config() reads
  // the safe 180s/unspecified-battery default on blank flash, a corrupt
  // page, or a begin() failure -- see ConfigStore::begin()'s contract.
  if (!config_store.begin()) {
    Serial.println(F("CONFIG unavailable; defaults in effect"));
  } else if (config_store.maintenanceResetRequired()) {
    Serial.println(F("CONFIG maintenance/reset required; defaults in effect"));
  } else if (config_store.tokenState() != orun_tlp::ConfigTokenState::kValid) {
    Serial.println(F("CONFIG token unavailable; defaults in effect"));
  }

  // M6D3C: recover the dedicated geofence resource while SoftDevice is still
  // disabled, then apply only its semantic snapshot to the existing M6D2
  // runtime owner. Token validity is intentionally not used as a synonym for
  // semantic readability: M6D3B may preserve CLEAR/CONFIGURED read-only
  // semantics while mutation/CAS authority is UNCERTAIN.
  if (!geofence_store.begin()) {
    Serial.println(F("GEOFENCE durable store unavailable; runtime unconfigured"));
  } else {
    if (!geofence_store.currentSnapshot(geofence_boot_snapshot)) {
      Serial.println(F("GEOFENCE durable authority unavailable; runtime unconfigured"));
    } else {
      const auto applied = orun_tlp::applyGeofenceSnapshotToRuntime(
          geofence_boot_snapshot, geofence_confirmation);
      if (applied == orun_tlp::GeofenceRuntimeApplyResult::kConfigured) {
        Serial.printf("GEOFENCE runtime configured areas=%u vertices=%u token=%s\n",
                      static_cast<unsigned>(geofence_boot_snapshot.area_count),
                      static_cast<unsigned>(geofence_boot_snapshot.total_vertex_count),
                      geofenceTokenStateName(geofence_store.tokenState()));
      } else if (applied == orun_tlp::GeofenceRuntimeApplyResult::kCleared) {
        Serial.printf("GEOFENCE runtime clear token=%s\n",
                      geofenceTokenStateName(geofence_store.tokenState()));
      } else {
        Serial.println(F("GEOFENCE durable snapshot rejected; runtime unconfigured"));
      }
    }
  }

  // M7P6B: recovery only -- never provisions a credential. See the
  // composition-root comment on security_store above.
#if defined(ORUN_M7P6I_HISTORY_CRYPTO_PROBE)
  // This test-only image must not mutate or even recover the physical
  // SecurityStore partition. Its KAT below uses a separate RAM-backed store
  // with public vector material. Leaving the production object unopened also
  // makes any accidental probe dependency on real credentials fail closed.
  Serial.println(F("SECURITY physical store skipped by M7P6I probe"));
#elif defined(ORUN_M4P5C_HISTORY_RECEIPT_PROBE)
  // M4P5C also uses a separate RAM-backed public SecurityStore for its
  // full receive-composition KAT. Do not make the test result depend on or
  // mutate the development unit's real credential partition.
  Serial.println(F("SECURITY physical store skipped by M4P5C probe"));
#else
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
#endif
  active_tracking_base_interval_seconds =
      config_store.config().tracking_interval_seconds;
  gnss_manager.begin();
  gnss_manager.setTrackingIntervalMs(
      active_tracking_base_interval_seconds * 1000UL);
  accelerometer_manager.begin(orun_tlp::monotonic::nowMs());
  Serial.println(F("ROLE AUTO pending (GNSS=>TRACKER, no GNSS=>BASE)"));

  // M7P7B: BLE starts last, strictly after History/Config/Geofence/Security
  // have finished their SoftDevice-disabled synchronous recovery above --
  // NrfHistoryFlash::begin() (and the Config/Geofence/Security equivalents) fail
  // closed if SoftDevice is already enabled when they run, and
  // Bluefruit.begin() is what enables SoftDevice for the rest of this boot
  // (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md §9: the sync/async
  // backend mode is fixed for the lifetime of one boot, not hot-swapped).
  // M7P7G keeps the same storage-before-SoftDevice ordering, then adds the
  // M7P7F read-only ORUN application GATT service after Bluefruit.begin().
  // This does not add commissioning/authorization/protected writes.
  ble_ready = Bluefruit.begin();
#ifdef ORUN_M4P5C_HISTORY_RECEIPT_PROBE
  if (ble_ready) {
    // Exercise the exact production crypto + opaque receipt + replay-admission
    // composition only after Bluefruit owns the shared CC310 lifecycle.
    // Persistence used by this KAT is RAM-backed public test state.
    m4p5c_history_receipt_kat =
        orun_tlp::m4p5c_test::runHistoryReceiptKat();
    m4p5c_history_receipt_kat_done = true;
    printM4P5CHistoryReceiptKat();
  } else {
    Serial.println(F("M4P5C HISTORY RECEIPT KAT FAIL reason=ble-not-ready"));
  }
#endif
#ifdef ORUN_M7P6I_HISTORY_CRYPTO_PROBE
  if (ble_ready) {
    // M7P6I calls the exact production HistorySecureCrypto implementation
    // after Bluefruit/SoftDevice owns the shared nRFCrypto/CC310 lifecycle.
    // The probe uses only RAM-backed public test credentials; no physical
    // SecurityStore partition is touched.
    m7p6i_history_crypto_kat =
        orun_tlp::m7p6i_test::runHistoryCryptoKat();
    m7p6i_history_crypto_kat_done = true;
    printM7P6IHistoryCryptoKat();
  } else {
    Serial.println(F("M7P6I HISTORY CRYPTO KAT FAIL reason=ble-not-ready"));
  }
#endif
#ifdef ORUN_M7P6E_CRYPTO_BLE_PROBE
  if (ble_ready) {
    // Bluefruit.begin() has already enabled SoftDevice and initialized the
    // shared nRFCrypto/CC310 facility. Do not call nRFCrypto.begin()/end()
    // from the ORUN probe. The KAT itself is the readiness canary required by
    // M7P6D; a true Bluefruit return alone is not sufficient evidence.
    const auto boot_kat = orun_tlp::m7p6e_test::runCandidateKat();
    m7p6e_boot_kat_pass = boot_kat.pass;
    printM7P6EKat("M7P6E BOOT KAT", boot_kat);
  }
#endif
  if (ble_ready) {
    char name[16];
    snprintf(name, sizeof(name), "ORUN-%08lX",
             static_cast<unsigned long>(device_identity.legacyUint64() & 0xFFFFFFFFUL));
    Bluefruit.setName(name);

    ble_application_gatt_ready = beginBleApplicationGatt();
    if (ble_application_gatt_ready)
      Serial.println(F("BLE APP GATT ready"));
    else
      Serial.println(F("BLE APP GATT unavailable"));

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
    // Minimal event handoff only; see onBleEvent() above.
    Bluefruit.setEventCallback(onBleEvent);
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
#ifdef ORUN_M7P6I_HISTORY_CRYPTO_PROBE
  if (m7p6i_history_crypto_kat_done && Serial &&
      (millis() - m7p6i_history_crypto_last_report_ms) >= 3000U) {
    printM7P6IHistoryCryptoKat();
  }
#endif
#ifdef ORUN_M4P5C_HISTORY_RECEIPT_PROBE
  if (m4p5c_history_receipt_kat_done && Serial &&
      (millis() - m4p5c_history_receipt_last_report_ms) >= 3000U) {
    printM4P5CHistoryReceiptKat();
  }
#endif
  // M6D2 deadline is checked BEFORE servicing another GNSS callback so an
  // observation arriving at/after the exact deadline cannot win a race and be
  // counted as confirmation evidence.
  const uint32_t loop_started_at_ms = orun_tlp::monotonic::nowMs();
  if (geofence_confirmation.expireConfirmation(loop_started_at_ms)) {
    (void)gnss_manager.cancelAdditionalFixAcquisition();
    clearGeofenceEpisodeFixes();
    Serial.println(F("GEOFENCE confirmation timeout"));
  }

  // GNSS detection/power remains owned by GnssManager. Service resolution must
  // not silently turn role, location source, GNSS power or accelerometer
  // presence into one knob.
  // Physical RAK4631 qualification showed that a HistoryStore page erase
  // accepted by SoftDevice can remain active across loop passes and overlap a
  // Wire transaction, tripping the bounded 25 ms TWIM timeout and forcing an
  // otherwise-unnecessary GNSS session resync. Quiesce the two loop-owned I2C
  // clients from the moment HistoryStore enters its erase phase until that
  // erase completes. Normal record programs are intentionally unaffected.
  if (!history.erasePending()) {
    gnss_manager.poll();
    handleAccelerometerEvent(
        accelerometer_manager.poll(orun_tlp::monotonic::nowMs()));
  }
  activity_capture.poll();
  pollRoleCommands();
  // M7P7D: transport input and application result consumption are separate
  // loop-owned steps. A future BLE callback may only enqueue/copy bounded
  // transport input; it must not execute application work itself.
  drainApplicationResponse();
#ifdef ORUN_M7P6E_CRYPTO_BLE_PROBE
  pollM7P6ECryptoStress();
#endif
  if (!automatic_role_resolved && role_controller.automatic() &&
      gnss_manager.detectionComplete()) {
    role_controller.updateAutomatic(true, gnss_manager.detected());
    automatic_role_resolved = true;
    applyRole(role_controller.role(), "AUTO");
  }

  const auto effective = resolveRuntimeConfig();
  const bool tracking_enabled = serviceRuns(effective.tracking);
  const bool relay_forwarding_enabled = serviceRuns(effective.relay_forwarding);
  if (!tracking_enabled && geofence_representative_pending) {
    // Do not retain a location indefinitely while the tracking service is
    // intentionally disabled. It may already have been accepted as geofence
    // evidence, but disabled tracking must not cause an hours-later history
    // insertion when the service is re-enabled.
    geofence_representative_pending = false;
    Serial.println(F("GEOFENCE representative discarded; tracking disabled"));
  }
  // Applying relay behavior may defer while a TX or legacy role transition is
  // active. The loop retries the same resolved intent without aborting work.
  radio_manager.setRelayForwardingEnabled(relay_forwarding_enabled);

  // M7P7B: drive the pure admission policy from the loop task only. Two
  // sources feed it: the polled Bluefruit state (keeps working for a
  // connection that stays up) and the disconnect event counter handed off by
  // onBleEvent() (catches a connect+disconnect that fit entirely between two
  // polls). BLE has its own physical radio (nRF52840 2.4GHz),
  // independent of the SX1262 LoRa radio_manager guards below, so this does
  // not need the TX guard.
  if (ble_ready) {
    taskENTER_CRITICAL();
    const uint32_t disconnect_events = ble_disconnect_events;
    taskEXIT_CRITICAL();
    orun_tlp::BleAdmissionInput input;
    input.disconnect_event = disconnect_events != ble_disconnect_events_seen;
    // Advertising is sampled before the connection: the framework clears
    // _running only after the connection object exists, so "not running"
    // read here can never pair with a stale "not connected".
    input.advertising_running = Bluefruit.Advertising.isRunning();
    input.connected = Bluefruit.Periph.connected() > 0;
    const uint16_t connection_handle =
        input.connected ? Bluefruit.connHandle() : BLE_CONN_HANDLE_INVALID;
    const uint32_t ble_now = orun_tlp::monotonic::nowMs();

    // M7P7G cleanup/transport work must happen before admission can restart
    // advertising after a disconnect.
    pollBleApplicationRuntime(input.connected, connection_handle,
                              input.disconnect_event, ble_now);
    ble_disconnect_events_seen = disconnect_events;

    switch (ble_admission.update(input, ble_now)) {
      case orun_tlp::BleAdmissionAction::kClose: {
        // The deadline only REQUESTS close. stop() can fail (pinned 1.7.0
        // leaves _running unchanged when sd_ble_gap_adv_stop() fails, e.g.
        // a connection racing the stop), so trust observed state, not the
        // return value: confirm only when advertising is really not running
        // and nobody is connected; otherwise the policy repeats the request.
        Bluefruit.Advertising.stop();
        const bool still_running = Bluefruit.Advertising.isRunning();
        const bool connected_now = Bluefruit.Periph.connected() > 0;
        // A whole connect+disconnect can complete between stop() and the two
        // reads above; then neither read shows the client but a real
        // disconnect is owed a fresh window. Read the counter AFTER the
        // connection state and leave the close unconfirmed: the next tick
        // consumes the event, cancels the close and restarts advertising.
        taskENTER_CRITICAL();
        const bool disconnect_pending =
            ble_disconnect_events != ble_disconnect_events_seen;
        taskEXIT_CRITICAL();
        if (!still_running && !connected_now && !disconnect_pending) {
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
  // History, Config (M7P5), Geofence (M6D3C) and Security (M7P6B) clients.
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
#ifdef ORUN_M7P7B_FLASH_PROBE
  // Stepped after config_store.poll() so a just-finished save is consumed on
  // the same tick. Idle/done: no sampling, no critical section, no output.
  if (flash_probe.state() != orun_tlp::ConfigFlashProbe::State::kIdle &&
      flash_probe.state() != orun_tlp::ConfigFlashProbe::State::kDone) {
    if (flash_probe.poll(config_store, flashProbeInputs())) {
      printFlashProbeReport();
      flash_probe.reset();
    }
  }
#endif
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
  // A confirmed transition may choose one of the real burst observations as
  // its immediate logical POSITION. Queue exactly one representative until the
  // existing store-before-send owner can accept it; do not bypass HistoryStore.
  if (tracking_enabled && geofence_representative_pending &&
      positions.prepareForFixStorage()) {
    if (positions.acceptPreviouslyAcceptedFix(
            geofence_representative_fix, orun_tlp::monotonic::nowMs())) {
      geofence_representative_pending = false;
    } else {
      // A real storage-admission failure is distinct from mere age. The
      // already-accepted representative is allowed to be historical, but it
      // must still never bypass HistoryStore.
      geofence_representative_pending = false;
      Serial.println(F("GEOFENCE representative storage failed"));
    }
  }

  orun_tlp::GnssFix fix{};
  const bool confirmation_fix_expected =
      geofence_confirmation.confirmationActive();

  // Preserve the old production gate when geofence is inactive/unconfigured:
  // normal fixes are consumed only when PositionFlow can accept them. During a
  // bounded confirmation episode, the two extra accepted fixes are local
  // evidence and must not be blocked by an unrelated in-flight History append.
  const bool normal_fix_storage_ready =
      tracking_enabled &&
      (confirmation_fix_expected ||
       (gnss_manager.hasFreshFixForTransmission() &&
        positions.prepareForFixStorage()));
  if (normal_fix_storage_ready &&
      gnss_manager.takeFreshFixForTransmission(&fix)) {
    // M7P7I observes the exact same already-accepted GNSS value once. Extend
    // its recent uint32 callback capture time into the shared long-lived
    // monotonic timeline before geofence processing can request another fix.
    const uint64_t location_now_ms = orun_tlp::monotonic::nowMs64();
    orun_tlp::AcceptedLocation accepted_location{};
    accepted_location.observed_monotonic_ms =
        orun_tlp::extendRecentMonotonicMs(location_now_ms, fix.captured_at_ms);
    accepted_location.utc_epoch_seconds = fix.utc_epoch_seconds;
    accepted_location.latitude_e7 = fix.latitude_e7;
    accepted_location.longitude_e7 = fix.longitude_e7;
    accepted_location.altitude_mm = fix.altitude_mm;
    accepted_location.source = orun_tlp::LocationSource::kGnss;
    // A promoted 2D navigation fix may carry an assumed/stale height even when
    // invalidLlh is clear. Publish altitude as trustworthy only for the existing
    // 3D / GNSS+dead-reckoning fix classes.
    accepted_location.altitude_valid =
        (fix.flags & orun_tlp::tlp::kPositionFlag3dFix) != 0;
    accepted_location.utc_valid =
        (fix.flags & orun_tlp::tlp::kPositionFlagValidUtcTime) != 0;
    if (!location_owner.accept(accepted_location)) {
      // Defensive invariant only. Never let the new read-only Location owner
      // gate the physically proven geofence/store-first path.
      Serial.println(F("LOCATION accepted fix invariant rejected"));
    }

    processGeofenceAcceptedFix(fix, !confirmation_fix_expected);

    if (!confirmation_fix_expected) {
      const bool accepted =
          positions.acceptFix(fix, orun_tlp::monotonic::nowMs());
      if (geofence_slot0_normal_store_expected) {
        geofence_slot0_normal_store_accepted = accepted;
        geofence_slot0_normal_store_expected = false;
      }
      if (!accepted)
        Serial.println(F("STORAGE position dropped; no live TX"));
    }
  }

  // If the acquisition owner crossed a recovery/timeout boundary while M6D2
  // still expected an extra observation, abort immediately rather than carry
  // evidence into a new GNSS session generation.
  if (geofence_confirmation.confirmationActive() &&
      !gnss_manager.additionalFixAcquisitionActive() &&
      gnss_manager.state() != orun_tlp::GnssManager::State::kFixAvailable) {
    abortGeofenceConfirmation("GNSS_SESSION_ENDED");
  }

  radio_manager.update(tracking_enabled && !positions.pending());
  // Feed only after the cooperative loop has completed all service work. A
  // blocked I2C/flash/radio path therefore cannot hide behind an unrelated task.
  orun_tlp::WatchdogManager::feed();
  orun_tlp::PowerManager::idle();
}
