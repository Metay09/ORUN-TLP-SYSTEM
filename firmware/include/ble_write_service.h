#pragma once

#include <stdint.h>

#include "ble_application_transport.h"
#include "ble_write_auth.h"
#include "config_mutation.h"
#include "config_store.h"
#include "device_storage_role.h"

namespace orun_tlp {

// 4c-1b: the Tracker's authenticated Bluetooth configuration writes. Glue
// between the BLE transport (framing), BleWriteAuthenticator (challenge,
// tags) and ConfigMutationOwner (the one owner of configuration changes):
// the same writer, bounds and outcomes as USB, only the admission differs.
// Loop-owned, fixed memory, host-tested end to end.
class BleWriteService : public BleWriteGate {
 public:
  BleWriteService(BleWriteAuthenticator& auth, ConfigMutationOwner& mutations,
                  const ConfigStore& store)
      : auth_(auth), mutations_(mutations), store_(store) {}

  // Composition facts, refreshed by the caller every loop: whether this
  // device may take authenticated writes at all (a provisioned Tracker with
  // a real credential) and the storage role a service write must fit.
  void setPolicy(bool writable, StorageRole role) {
    writable_ = writable;
    role_ = role;
  }

  // BleWriteGate
  bool challenge(uint32_t now, uint8_t* payload, uint16_t& length,
                 ble_app_transport::ErrorCode& error) override;
  BleWriteAdmission submitWrite(uint32_t now, uint32_t request_id,
                                const uint8_t* payload,
                                uint16_t length) override;
  void sessionReset() override;

  enum class Answer : uint8_t {
    kNone = 0,     // nothing finished
    kReady = 1,    // `payload` is the signed answer for `request_id`
    kDropped = 2,  // a write finished, but its session is gone
  };

  // Call every loop after ConfigMutationOwner::poll() (and after a committed
  // change reached the runtime). Always takes a BLE-owned mutation result so
  // the single mutation slot is freed even after a disconnect.
  Answer takeAnswer(
      uint32_t& request_id,
      uint8_t (&payload)[ble_app_transport::kWriteResultPayloadSize],
      ConfigMutationResult& result);

 private:
  void prepareImmediate(uint32_t request_id, uint8_t op,
                        ConfigMutationOutcome outcome);
  bool build(const ConfigMutationResult& result, uint8_t op,
             uint8_t (&payload)[ble_app_transport::kWriteResultPayloadSize]);

  BleWriteAuthenticator& auth_;
  ConfigMutationOwner& mutations_;
  const ConfigStore& store_;
  bool writable_ = false;
  StorageRole role_ = StorageRole::kUnassigned;

  // A write handed to ConfigMutationOwner and not answered yet.
  bool owed_ = false;
  uint32_t owed_request_id_ = 0;
  uint8_t owed_op_ = 0;
  // An authenticated write refused before reaching the owner (unknown op,
  // service intent for the other role): answered, signed, next takeAnswer().
  bool immediate_ = false;
  uint8_t immediate_op_ = 0;
  ConfigMutationResult immediate_result_{};
};

}  // namespace orun_tlp
