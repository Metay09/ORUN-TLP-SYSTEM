#pragma once

#include <stddef.h>
#include <stdint.h>

#include "security_format.h"
#include "tlp_v2_history_secure.h"

namespace orun_tlp {

class SecurityStore;

enum class HistorySecureCryptoResult : uint8_t {
  kOk,
  kUnavailable,
  kInvalidArgument,
  kAuthRejected,
  kEngineError,
};

// Narrow root-credential crypto boundary for the frozen M4P4/SF2 History wire.
//
// K_root remains owned by SecurityStore. This class has friend access only so
// protected traffic can use the active credential without introducing a public
// or remote key-readback API.
//
// M7P6I adds no RF/runtime caller. It only supplies the production crypto seam
// needed before SF3 can enable protected History replay.
class HistorySecureCrypto {
 public:
  explicit HistorySecureCrypto(SecurityStore& security_store)
      : security_store_(security_store) {}

  // Reserves/consumes the D2A security counter from SecurityStore itself.
  // Callers cannot supply an arbitrary counter; nonce ownership remains with
  // the durable security owner.
  HistorySecureCryptoResult protectNextObservation(
      uint64_t history_incarnation, uint8_t path_flags,
      const tlp::HistoryObservationPlaintext& observation,
      tlp::HistorySecurePacket& packet);

  // Authenticates/decrypts a complete frozen SF2 BACKEND_DURABLE receipt frame.
  // On any failure, packet/receipt/authenticated_credential_id outputs are left
  // untouched.
  //
  // This does NOT mutate SecurityStore A2D replay state or History delivery
  // state. The future SF3 owner MUST submit exactly:
  //   authenticated_credential_id,
  //   packet.key_epoch,
  //   packet.security_counter
  // to SecurityStore after kOk. It MUST NOT re-read the current credential,
  // because credential rotation may occur before a deferred replay submission.
  HistorySecureCryptoResult openBackendDurableReceipt(
      const uint8_t* frame, size_t frame_size,
      uint64_t expected_history_incarnation,
      tlp::HistorySecurePacket& packet,
      tlp::BackendDurableReceiptPlaintext& receipt,
      uint8_t (&authenticated_credential_id)
          [security_format::kCredentialIdSize]);

 private:
  SecurityStore& security_store_;
};

}  // namespace orun_tlp
