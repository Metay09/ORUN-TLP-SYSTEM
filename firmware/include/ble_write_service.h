#pragma once

#include <stdint.h>

#include "ble_application_transport.h"
#include "ble_write_auth.h"
#include "config_mutation.h"
#include "config_store.h"
#include "device_storage_role.h"
#include "geofence_mutation.h"
#include "geofence_staging.h"
#include "geofence_store.h"

namespace orun_tlp {

// 4c-1b / 4c-2: the Tracker's authenticated Bluetooth writes. Glue between
// the BLE transport (framing), BleWriteAuthenticator (challenge, tags) and
// the one owner of each resource: ConfigMutationOwner for settings,
// GeofenceMutationOwner for the area list. Same writers, bounds and outcomes
// as USB; only the admission differs. Loop-owned, fixed memory, host-tested
// end to end.
class BleWriteService : public BleWriteGate {
 public:
  BleWriteService(BleWriteAuthenticator& auth, ConfigMutationOwner& mutations,
                  const ConfigStore& store,
                  GeofenceMutationOwner& fence_mutations,
                  const GeofenceStore& fences)
      : auth_(auth), mutations_(mutations), store_(store),
        fence_mutations_(fence_mutations), fences_(fences) {}

  // Composition facts, refreshed by the caller every loop: whether this
  // device may take authenticated writes at all (a provisioned Tracker with
  // a real credential) and the storage role a service write must fit.
  void setPolicy(bool writable, StorageRole role) {
    writable_ = writable;
    role_ = role;
    if (!writable) staging_.abort();
  }

  // BleWriteGate
  bool challenge(uint32_t now, uint8_t* payload, uint16_t& length,
                 ble_app_transport::ErrorCode& error) override;
  BleWriteAdmission submitWrite(uint32_t now, uint32_t request_id,
                                const uint8_t* payload,
                                uint16_t length) override;
  bool stageFence(const uint8_t* payload, uint16_t length, uint8_t* answer,
                  uint16_t& answer_length,
                  ble_app_transport::ErrorCode& error) override;
  void sessionReset() override;

  enum class Answer : uint8_t {
    kNone = 0,     // nothing finished
    kReady = 1,    // `payload` is the signed answer for `request_id`
    kDropped = 2,  // a write finished, but its session is gone
  };

  // Call every loop after the mutation owners' poll() (and after a committed
  // change reached the runtime). Always takes a BLE-owned result so each
  // single mutation slot is freed even after a disconnect. `payload` holds
  // the answer bytes (also when dropped: for the log).
  Answer takeAnswer(
      uint32_t& request_id,
      uint8_t (&payload)[ble_app_transport::kWriteResultPayloadSize]);

  const GeofenceStaging& staging() const { return staging_; }

 private:
  using Body = uint8_t[ble_app_transport::kWriteResultBodySize];
  void configBody(ConfigMutationOutcome outcome, uint8_t op,
                  const config_format::Config& config, bool token_valid,
                  uint32_t revision, Body& body) const;
  void fenceBody(ConfigMutationOutcome outcome, GeofenceResourceState state,
                 uint8_t areas, uint16_t vertices, bool token_valid,
                 uint32_t revision, Body& body) const;
  void immediate(uint32_t request_id, const Body& body);
  bool stagedDigest(uint8_t (&digest)[32]);
  Answer finish(uint32_t request_id, const Body& body,
                uint8_t (&payload)[ble_app_transport::kWriteResultPayloadSize]);

  BleWriteAuthenticator& auth_;
  ConfigMutationOwner& mutations_;
  const ConfigStore& store_;
  GeofenceMutationOwner& fence_mutations_;
  const GeofenceStore& fences_;
  bool writable_ = false;
  StorageRole role_ = StorageRole::kUnassigned;
  // BLE-only area list candidate (USB keeps its own).
  GeofenceStaging staging_;

  // A write handed to an owner and not answered yet.
  enum class Owed : uint8_t { kNone, kConfig, kFence };
  Owed owed_ = Owed::kNone;
  uint32_t owed_request_id_ = 0;
  uint8_t owed_op_ = 0;
  // An authenticated write refused before reaching an owner (unknown op,
  // another role's service, a list that is not what was staged): answered,
  // signed, at the next takeAnswer().
  bool immediate_ = false;
  uint32_t immediate_request_id_ = 0;
  Body immediate_body_{};
  // Canonical staged set for the digest (areas, per area n + n points).
  uint8_t canonical_[1 + geofence_format::kMaximumAreas +
                     8 * geofence_format::kMaximumTotalVertices]{};
};

}  // namespace orun_tlp
