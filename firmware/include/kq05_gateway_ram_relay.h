#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE

namespace orun_tlp::kq05_ram_relay {

// KQ05 LAB; a Gateway RAM cache is NOT the SF5L6 durable custody store.
// It MUST NOT grant an Edge STAGED response, a Tracker ACK, or erase authority.
// Concrete Edge credential enrollment is NOT implemented in this slice.
class CredentialSource {
 public:
  virtual ~CredentialSource() {}
  // TRUE only for a persistent, revocable, locally verified Gateway/Edge
  // enrollment. Never use a shared test key or the Tracker's K_root.
  virtual bool verifiedForGateway(uint64_t gateway_id) const = 0;
  // Bounded ASCII Edge installation ID (1..64), dedicated 32B link key.
  virtual bool readGatewayLink(uint64_t gateway_id,
                               char (&edge_id)[65],
                               uint8_t (&link_key)[32]) const = 0;
  virtual bool permitsTracker(uint64_t gateway_id,
                              uint64_t tracker_id) const = 0;
};

enum class Admission : uint8_t {
  kQueued, kDuplicate, kFull, kDenied, kUnavailable
};

struct TxConditions {
  bool gateway_service_authorized = false;
  bool radio_idle = false;
  bool custody_ack_pending = true;
  bool command_pending = true;
  bool live_pending = true;
  bool regional_rf_budget_granted = false;
  bool tracker_rx_window_authorized = false;
  uint64_t heard_tracker = 0U;
  uint32_t heard_age_ms = UINT32_MAX;
};

struct Candidate {
  uint64_t source_row = 0U;
  uint64_t tracker = 0U;
  uint8_t wire[72]{};
  uint8_t length = 0U;
};

class GatewayRamRelay final {
 public:
  // Only two 72B receipt slots; no internal Flash, no evict-while-live.
  static constexpr size_t kCapacity = 2U;
  static constexpr uint32_t kTtlMs = 20000U;
  static constexpr uint32_t kFreshRxHintMs = 3000U;
  static constexpr uint32_t kMinRetryMs = 30000U;

  explicit GatewayRamRelay(uint64_t gateway_id) : gateway_id_(gateway_id) {}
  Admission accept(const uint8_t* logical_message, size_t length,
                   uint32_t now_ms, bool gateway_service_authorized,
                   const CredentialSource& credentials);
  // Eligibility is only a read-only candidate selection. Caller must still
  // verify hardware role and acquire the RadioManager's actual TX gate.
  bool select(uint32_t now_ms, const TxConditions& policy,
              const CredentialSource& credentials, Candidate& output);
  // Called ONLY after the real radio TX owner accepted a launch. This
  // imposes local retry pacing; it does not mean RF delivery occurred.
  void markTxStarted(uint64_t source_row, uint32_t now_ms);
  void reset();
  size_t pending(uint32_t now_ms);

 private:
  struct Entry {
    uint64_t source_row = 0U;
    uint64_t tracker = 0U;
    uint8_t wire[72]{};
    uint8_t length = 0U;
    uint32_t since_ms = 0U;
    uint32_t last_attempt_ms = 0U;
    bool used = false;
    bool attempted = false;
  };

  void expire(uint32_t now_ms);
  static void zero(Entry& entry);
  uint64_t gateway_id_;
  Entry entries_[kCapacity]{};
};

}  // namespace orun_tlp::kq05_ram_relay

#endif  // ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
