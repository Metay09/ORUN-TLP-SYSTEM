#pragma once

#include <stdint.h>

#include "gateway_custody_authorized_ingress.h"
#include "gateway_product_startup_guard.h"
#include "radio_manager.h"
#ifdef ORUN_GATEWAY_BATCH_ACK
#include "gateway_product_live_ingress.h"
#include "gateway_grant_snapshot_reader.h"
#endif

namespace orun_tlp {

// Gateway application-loop glue for the EXISTING custody owner and RF owner.
// The trusted composition root must independently mount an OCQ2-only product
// region and provide durable, provisioned GatewayCustodyGrantProvider grants.
// This class neither provisions keys nor grants Gateway authority to BASE.
class GatewayProductCustodyRuntime final {
 public:
  enum class Event : uint8_t {
    kNone, kCustodyStarted, kDuplicateHeld, kDenied, kAckTxStarted, kLiveDurable
  };

  GatewayProductCustodyRuntime(RadioManager& radio,
                               GatewayCustodyAuthorizedIngress& ingress)
      : radio_(radio), ingress_(ingress) {}

  // Call after RadioManager::update() in the SAME loop task as grant mutation.
  // enabled must be granted by an authenticated Gateway service owner, NEVER
  // derived solely from NodeRole::kBase or reception of a radio packet.
  // Pending flash completions still progress when service is disabled;
  // disabled/mismatched-identity instances never receive or transmit.
  Event update(bool enabled);

#ifdef ORUN_GATEWAY_BATCH_ACK
  void attachLiveIngress(GatewayProductLiveIngress& live) { live_ = &live; }
  uint32_t reservedPages() const;
  void reserveCommandWindow(bool reserved) { command_window_reserved_ = reserved; }
#endif

  // Future production composition MUST use the verified boot gate; the
  // boolean overload remains the existing low-level SF5K3W host seam.
  Event updateGuarded(const GatewayProductStartupGuard& gate,
                      bool authorized_gateway_service,
                      bool observation_store_mounted) {
    return update(gate.mayServe(authorized_gateway_service,
                                observation_store_mounted, radio_.deviceId()));
  }

  // SF5L6: RSSI/SNR of the frame whose custody most recently STARTED (how
  // strongly this Gateway heard the Tracker). Diagnostics only; no authority.
  int16_t lastCustodyRssi() const { return last_rssi_; }
  int8_t lastCustodySnr() const { return last_snr_; }

 private:
  RadioManager& radio_;
  GatewayCustodyAuthorizedIngress& ingress_;
#ifdef ORUN_GATEWAY_BATCH_ACK
  struct PeerBatch {
    GatewayCustodyAckScope scope{};
    ProductCustodyStore::Handle handles[kq05_gateway_batch_ack::kMaxObjects]{};
    uint32_t first_ms = 0;
    uint32_t heard_ms = 0;
    uint8_t count = 0;
    uint32_t last_attempt_ms = 0;
    uint8_t attempts = 0;
    bool individual = false;
    bool urgent = false;
  };
  PeerBatch peers_[gateway_grant_format::kGrantSlots]{};

  static_assert(gateway_grant_format::kGrantSlots <= UINT8_MAX,
                "ACK cursor cannot address every peer");

  // Volatile scheduling hint only. Never custody authority.
  uint8_t next_ack_peer_ = 0U;
  uint32_t pending_rx_ms_ = 0;
  GatewayProductLiveIngress* live_ = nullptr;
  bool command_window_reserved_ = false;
  bool pending_event_ = false;
  void collectBatchHandle(uint32_t now);
#endif
  int16_t last_rssi_ = 0;
  int8_t last_snr_ = 0;
};

}  // namespace orun_tlp
