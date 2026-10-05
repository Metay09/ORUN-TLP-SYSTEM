#pragma once

#include <stddef.h>
#include <stdint.h>

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

  HistorySecureCryptoResult protectObservation(
      uint64_t history_incarnation, uint64_t security_counter,
      uint8_t path_flags,
      const tlp::HistoryObservationPlaintext& observation,
      tlp::HistorySecurePacket& packet);

  // Authenticates/decrypts a complete frozen SF2 BACKEND_DURABLE receipt frame.
  // On any failure, packet/receipt outputs are left untouched.
  //
  // This does NOT mutate SecurityStore A2D replay state or History delivery
  // state. The future SF3 owner must submit packet.security_counter to
  // SecurityStore only after kOk is returned.
  HistorySecureCryptoResult openBackendDurableReceipt(
      const uint8_t* frame, size_t frame_size,
      uint64_t expected_history_incarnation,
      tlp::HistorySecurePacket& packet,
      tlp::BackendDurableReceiptPlaintext& receipt);

 private:
  SecurityStore& security_store_;
};

}  // namespace orun_tlp
