#pragma once
#include <stdint.h>

#ifdef ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF
#include "gateway_product_live_ingress.h"
#include "gateway_product_startup_guard.h"
#include "kq05_gateway_batch_collector.h"
#include "radio_manager.h"

namespace orun_tlp {

// Experimental, loop-owned Gateway 0x0A composition. The collector owns
// exactly 1..5 confirmed-durable OCQ2 handles for ONE Tracker/epoch.
// The independent LIVE ingress is never blocked merely because the group is
// full or its downlink is waiting. Neither path grants Backend delivery.
//
// This takes the place of the old one-record Gateway custody runtime ONLY
// when ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF is explicitly enabled.
// Flash polling and release outcome ownership remain serialized on one loop.
class Kq05GatewayBatchRuntime final {
 public:
  enum class Event : uint8_t {
    kNone, kCustodyStarted, kCustodyDurable, kDuplicate,
    kCustodyDeferred, kDenied, kAckTxStarted,
    kLiveStarted, kLiveDurable, kLiveDuplicate, kLiveDenied
  };

  // Provisional maximum wait after FIRST durable group item, not per-item
  // ACK timing. FIVE items flush immediately. Must be RF/power qualified
  // against Tracker downlink listen windows before any field activation.
  static constexpr uint32_t kDefaultPartialFlushMs = 120000U;

  Kq05GatewayBatchRuntime(RadioManager& radio,
                          Kq05GatewayBatchCollector& collector,
                          GatewayProductLiveIngress& live,
                          uint32_t partial_flush_ms = kDefaultPartialFlushMs,
                          bool expedite_events = true)
      : radio_(radio), collector_(collector), live_(live),
        partial_flush_ms_(partial_flush_ms==0U
            ? kDefaultPartialFlushMs : partial_flush_ms),
        expedite_events_(expedite_events) {}

  Event update(bool enabled, uint32_t now_ms);
  Event updateGuarded(const GatewayProductStartupGuard& gate,
                      bool authorized_gateway_service,
                      bool observation_store_mounted, uint32_t now_ms) {
    return update(gate.mayServe(authorized_gateway_service,
                                observation_store_mounted,
                                radio_.deviceId()), now_ms);
  }

  uint8_t queued() const { return collector_.queued(); }
  bool partialTimerArmed() const { return timer_armed_; }
  uint32_t partialWaitMs() const { return partial_flush_ms_; }
  uint32_t firstDurableMs() const { return first_durable_ms_; }
  int16_t lastCustodyRssi() const { return last_rssi_; }
  int8_t lastCustodySnr() const { return last_snr_; }

 private:
  void syncTimer(uint32_t now_ms);
  RadioManager& radio_;
  Kq05GatewayBatchCollector& collector_;
  GatewayProductLiveIngress& live_;
  uint32_t partial_flush_ms_;
  // EVENT custody gets a one-object 0x0A acknowledgement immediately after
  // durable OCQ2 commit; 1..5 batching remains available for non-EVENT.
  bool expedite_events_=true;
  bool event_write_pending_=false;
  uint32_t first_durable_ms_=0U;
  bool timer_armed_=false;
  int16_t last_rssi_=0;
  int8_t last_snr_=0;
};

} // namespace orun_tlp
#endif // ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF
