#include "v2_relay_forward_queue.h"

#include <string.h>

#include "custody_ack.h"
#include "monotonic_time.h"

namespace orun_tlp {
namespace {

bool sameInner(const tlp::V2RelayForward& a,
               const tlp::V2RelayForward& b) {
  return a.inner_length == b.inner_length &&
         memcmp(a.inner, b.inner, a.inner_length) == 0;
}

bool ack(const tlp::V2RelayForward& p) {
  return p.inner_length >= 2U && p.inner[1] == custody_ack::kType;
}

}  // namespace

V2RelayForwardQueue::Offer V2RelayForwardQueue::offer(
    const uint8_t* inner, size_t length, int16_t rssi, int8_t snr,
    uint32_t due_ms) {
  if (length > tlp::kV2RelayForwardMaxInnerSize ||
      tlp::validateV2RelayInner(inner, length) !=
          tlp::V2RelayCodecStatus::kOk)
    return Offer::kInvalid;

  for (const Entry& item : entries_)
    if (item.used && item.packet.inner_length == length &&
        memcmp(item.packet.inner, inner, length) == 0)
      return Offer::kDuplicate;

  if (queued_ >= kCapacity) return Offer::kFull;
  for (Entry& item : entries_) {
    if (item.used) continue;
    item = Entry{};
    item.used = true;
    item.due_ms = due_ms;
    item.packet.inner_length = static_cast<uint8_t>(length);
    item.packet.ingress_rssi_dbm = rssi;
    item.packet.ingress_snr_db = snr;
    memcpy(item.packet.inner, inner, length);
    ++queued_;
    return Offer::kQueued;
  }
  return Offer::kFull;
}

bool V2RelayForwardQueue::peekDue(
    uint32_t now_ms, tlp::V2RelayForward& out) const {
  const Entry* best = nullptr;
  for (const Entry& item : entries_) {
    if (!item.used || !monotonic::reached(now_ms, item.due_ms)) continue;
    if (best == nullptr || (ack(item.packet) && !ack(best->packet)) ||
        (ack(item.packet) == ack(best->packet) &&
         uint32_t(now_ms - item.due_ms) >
             uint32_t(now_ms - best->due_ms)))
      best = &item;
  }
  if (best == nullptr) return false;
  out = best->packet;
  return true;
}

bool V2RelayForwardQueue::consume(
    const tlp::V2RelayForward& selected) {
  for (Entry& item : entries_) {
    if (!item.used || !sameInner(item.packet, selected) ||
        item.packet.ingress_rssi_dbm != selected.ingress_rssi_dbm ||
        item.packet.ingress_snr_db != selected.ingress_snr_db)
      continue;
    item = Entry{};
    --queued_;
    return true;
  }
  return false;
}

void V2RelayForwardQueue::clear() {
  for (Entry& item : entries_) item = Entry{};
  queued_ = 0U;
}

}  // namespace orun_tlp
