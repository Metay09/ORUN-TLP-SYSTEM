#include "kq05_gateway_batch_runtime.h"

#ifdef ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF
#include <string.h>

#include "tlp_v2_product_secure.h"

namespace orun_tlp {

void Kq05GatewayBatchRuntime::syncTimer(uint32_t now_ms) {
  if (collector_.queued()==0U) {
    timer_armed_=false;
    first_durable_ms_=0U;
    retry_timer_armed_=false;
    first_retry_ms_=0U;
  } else if (!timer_armed_) {
    // Start only once: additional records cannot postpone partial ACK.
    first_durable_ms_=now_ms;
    timer_armed_=true;
  }
}

Kq05GatewayBatchRuntime::Event
Kq05GatewayBatchRuntime::update(bool enabled, uint32_t now_ms) {
  // Both owners use the SAME OCQ2 Flash mutation result. The loop MUST
  // never allow them to poll/take a single unfinished write concurrently.
  if (live_.pending() && collector_.pendingFlash())
    return Event::kNone; // impossible under this owner; fail closed.

  // Complete any already-admitted Flash operation even if the service
  // has since become disabled (revoked / storage-role change).
  if (live_.pending()) {
    const auto result=live_.service();
    if (result==GatewayProductLiveIngress::Service::kDurable)
      return Event::kLiveDurable;
    if (result==GatewayProductLiveIngress::Service::kFailed)
      return Event::kLiveDenied;
    return Event::kNone;
  }
  if (collector_.pendingFlash()) {
    const auto result=collector_.service();
    if (result==Kq05GatewayBatchCollector::Service::kQueued) {
      const bool expedite=expedite_events_ && event_write_pending_;
      event_write_pending_=false;
      syncTimer(now_ms);
      // A critical EVENT must not wait for five other packets or the
      // provisional two-minute partial-group timeout. Authenticated ACK
      // still waits for this exact record's confirmed durable readback.
      if (expedite || (trusted_sparse_single_ack_ &&
                      collector_.queued()==1U))
        (void)collector_.requestFlush();
      return Event::kCustodyDurable;
    }
    if (result==Kq05GatewayBatchCollector::Service::kRejected) {
      event_write_pending_=false;
      return Event::kDenied;
    }
    return Event::kNone;
  }

  syncTimer(now_ms);
  if (timer_armed_ && !collector_.flushRequested() &&
      static_cast<uint32_t>(now_ms-first_durable_ms_) >= partial_flush_ms_)
    (void)collector_.requestFlush();
  if (retry_timer_armed_ && !collector_.flushRequested() &&
      static_cast<uint32_t>(now_ms-first_retry_ms_) >= kRetryCoalesceMs)
    (void)collector_.requestFlush();

  // No RX and no downlink on an unverified boot, wrong hardware Gateway ID
  // or revoked service. A retained group is not erased on disable; only a
  // fresh still-authorized signer's grant can ever emit its 0x0A ACK.
  if (!enabled || collector_.localGatewayId()==0U ||
      radio_.deviceId()!=collector_.localGatewayId() ||
      live_.gatewayId()!=collector_.localGatewayId())
    return Event::kNone;

  // An unrelated Edge handoff/reclaim may own the Flash journal. Do not
  // steal its result or claim a new LIVE/CUSTODY write in this pass.
  if (live_.storageBusy()) return Event::kNone;

  // Take a newly received frame BEFORE sending a due group ACK. This makes
  // fresh LIVE eligible even when a 5-object group is already full or TX is
  // temporarily unavailable. RadioManager owns just one bounded mailbox.
  ProductSecureRxFrame raw{};
  if (radio_.takeProductSecureFrame(&raw)) {
    tlp::ProductSecureFrame decoded{};
    if (tlp::decodeProductSecureFrame(raw.payload,raw.size,&decoded)!=
        tlp::ProductSecureCodecResult::kOk) {
      memset(raw.payload,0,sizeof(raw.payload));
      return Event::kDenied;
    }
    if ((decoded.path_flags & tlp::kProductPathFlagCustodyRequested)==0U) {
      const auto outcome=live_.receive(raw.payload,raw.size);
      memset(raw.payload,0,sizeof(raw.payload));
      switch (outcome) {
        case GatewayProductLiveIngress::Receive::kStarted:
          return Event::kLiveStarted;
        case GatewayProductLiveIngress::Receive::kDuplicateDurable:
          return Event::kLiveDuplicate;
        case GatewayProductLiveIngress::Receive::kBusy:
          return Event::kNone; // no claimed delivery
        case GatewayProductLiveIngress::Receive::kDenied:
          return Event::kLiveDenied;
      }
    }

    const bool is_event=decoded.app_family==tlp::kProductAppFamilyEvent;
    // An urgent EVENT must never join a previously queued telemetry group.
    // Flush that group first; the Tracker retains its exact EVENT and
    // retries later. This keeps each EVENT's 0x0A ACK at count=1.
    if (expedite_events_ && is_event && collector_.queued()>0U) {
      (void)collector_.requestFlush();
      memset(raw.payload,0,sizeof(raw.payload));
      return Event::kCustodyDeferred;
    }
    const auto outcome=collector_.receive(raw.payload,raw.size);
    memset(raw.payload,0,sizeof(raw.payload));
    switch (outcome) {
      case Kq05GatewayBatchCollector::Receive::kStartedFlash:
        event_write_pending_=is_event;
        last_rssi_=raw.rssi_dbm;
        last_snr_=raw.snr_db;
        return Event::kCustodyStarted;
      case Kq05GatewayBatchCollector::Receive::kReenrolled:
        // An exact durable record is being retried after a lost downlink.
        // Give an RF-limited retransmission burst a short opportunity to
        // become a 2..5 item ACK, without waiting for a full group or
        // resetting this deadline when another retry arrives.
        syncTimer(now_ms);
        if (trusted_sparse_single_ack_ && collector_.queued()==1U) {
          (void)collector_.requestFlush();
        } else if (!retry_timer_armed_) {
          first_retry_ms_=now_ms;
          retry_timer_armed_=true;
        }
        return Event::kDuplicate;
      case Kq05GatewayBatchCollector::Receive::kQueued:
      case Kq05GatewayBatchCollector::Receive::kDuplicate:
        syncTimer(now_ms);
        if ((expedite_events_ && is_event) ||
            (trusted_sparse_single_ack_ && collector_.queued()==1U))
          (void)collector_.requestFlush();
        return Event::kDuplicate;
      case Kq05GatewayBatchCollector::Receive::kBusy:
        // Most importantly, a foreign Tracker cannot indefinitely extend
        // the existing group's deadline. Flush the prior group and let the
        // foreign Tracker retry its retained authenticated exact frame.
        if (!collector_.flushRequested() && collector_.queued()>0U)
          (void)collector_.requestFlush();
        return Event::kCustodyDeferred;
      case Kq05GatewayBatchCollector::Receive::kDenied:
        return Event::kDenied;
    }
  }

  // The RadioManager re-reads OCQ2, re-verifies current scoped grant and
  // HMAC before TX. Only after SX126x FIFO admission may the collector
  // forget its VOLATILE handles; durable OCQ2 data is NEVER discarded here.
  if (collector_.flushRequested() && radio_.canSend() &&
      radio_.sendAuthorizedKq05GatewayBatchAck(collector_)) {
    syncTimer(now_ms);
    return Event::kAckTxStarted;
  }

  return Event::kNone;
}

} // namespace orun_tlp
#endif
