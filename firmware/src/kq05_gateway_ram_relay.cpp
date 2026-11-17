#include "kq05_gateway_ram_relay.h"

#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE

#include <string.h>

#include "custody_ack_primitives.h"
#include "kq05_edge_link_wire.h"

namespace orun_tlp::kq05_ram_relay {
namespace {
constexpr char kMacDomain[] = "ORUN-KQ05-EDGE-GATEWAY-OFFER-v1";
// The Python protocol includes this domain's terminating NUL byte.
// MAC = HMAC(key, domain\0 || ASCII(edge_id) || \0 || offer_without_32B_MAC).
static_assert(sizeof(kMacDomain) == 32U, "KQ05 link MAC domain");
constexpr size_t kMaxMacData = sizeof(kMacDomain) + 64U + 1U +
                               (123U - 32U);
static_assert(kMaxMacData <= custody_crypto::kMaxBoundedData,
              "KQ05 offer HMAC exceeds CC310 bounded input");

uint32_t be32(const uint8_t* data) {
  return (uint32_t(data[0]) << 24U) | (uint32_t(data[1]) << 16U) |
         (uint32_t(data[2]) << 8U) | uint32_t(data[3]);
}

uint64_t be64(const uint8_t* data) {
  uint64_t value = 0U;
  for (uint8_t index = 0U; index < 8U; ++index)
    value = (value << 8U) | data[index];
  return value;
}

void wipe(void* data, size_t length) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(data);
  while (length-- != 0U) *p++ = 0U;
}

bool same(const uint8_t* a, const uint8_t* b, size_t length) {
  uint8_t difference = 0U;
  for (size_t i = 0U; i < length; ++i)
    difference = static_cast<uint8_t>(difference | (a[i] ^ b[i]));
  return difference == 0U;
}

bool validEdgeId(const char (&id)[65], size_t& length) {
  length = 0U;
  for (; length < 65U; ++length) {
    const unsigned char ch = static_cast<unsigned char>(id[length]);
    if (ch == 0U) return length != 0U;
    if (length == 64U ||
        !((ch >= 'a' && ch <= 'z') ||
          (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') ||
          ch == '-' || ch == '_'))
      return false;
  }
  return false;
}

bool macAuthorized(const uint8_t* message, size_t length,
                   const CredentialSource& source, uint64_t gateway_id) {
  if (message == nullptr || length < kq05_edge_link::kMinOfferMessageBytes ||
      length > kq05_edge_link::kMaxOfferMessageBytes)
    return false;
  char edge_id[65]{};
  uint8_t key[32]{};
  uint8_t scratch[kMaxMacData]{};
  uint8_t digest[32]{};
  bool ok = false;
  do {
    if (!source.readGatewayLink(gateway_id, edge_id, key))
      break;
    size_t id_length = 0U;
    if (!validEdgeId(edge_id, id_length)) break;
    uint8_t nonzero = 0U;
    for (uint8_t x : key) nonzero = static_cast<uint8_t>(nonzero | x);
    if (nonzero == 0U) break;
    // Remove the external BLE type byte, then omit the trailing Edge MAC.
    const size_t inner_unsigned = length - 1U - 32U;
    const size_t total = sizeof(kMacDomain) + id_length + 1U + inner_unsigned;
    if (total > sizeof(scratch)) break;
    memcpy(scratch, kMacDomain, sizeof(kMacDomain));
    memcpy(scratch + sizeof(kMacDomain), edge_id, id_length);
    // scratch is zero-initialized; the next byte is exactly \0.
    memcpy(scratch + sizeof(kMacDomain) + id_length + 1U,
           message + 1U, inner_unsigned);
    ok = custody_crypto::hmacSha256Bounded(key, scratch, total, digest) &&
         same(digest, message + length - 32U, 32U);
  } while (false);
  wipe(edge_id, sizeof(edge_id));
  wipe(key, sizeof(key));
  wipe(scratch, sizeof(scratch));
  wipe(digest, sizeof(digest));
  return ok;
}
}  // namespace

void GatewayRamRelay::zero(Entry& entry) {
  wipe(&entry, sizeof(entry));
}

void GatewayRamRelay::reset() {
  for (size_t i = 0U; i < kCapacity; ++i) zero(entries_[i]);
}

void GatewayRamRelay::expire(uint32_t now_ms) {
  for (size_t i = 0U; i < kCapacity; ++i)
    if (entries_[i].used &&
        static_cast<uint32_t>(now_ms - entries_[i].since_ms) >= kTtlMs)
      zero(entries_[i]);
}

size_t GatewayRamRelay::pending(uint32_t now_ms) {
  expire(now_ms);
  size_t count = 0U;
  for (size_t i = 0U; i < kCapacity; ++i)
    if (entries_[i].used) ++count;
  return count;
}

Admission GatewayRamRelay::accept(
    const uint8_t* message, size_t length, uint32_t now_ms,
    bool gateway_service_authorized, const CredentialSource& source) {
  expire(now_ms);
  if (!gateway_service_authorized || gateway_id_ == 0U ||
      !source.verifiedForGateway(gateway_id_)) {
    reset();  // Revoked or disabled: a prior link grant cannot survive.
    return Admission::kDenied;
  }

  kq05_edge_link::UntrustedOfferView offer{};
  if (!kq05_edge_link::inspectOffer(message, length, offer) ||
      be64(offer.gateway) != gateway_id_)
    return Admission::kDenied;
  const uint64_t tracker = be64(offer.receipt + 4U);
  const uint32_t key_epoch = be32(offer.receipt + 20U);
  if (tracker == 0U || key_epoch == UINT32_MAX ||
      !source.permitsTracker(gateway_id_, tracker, key_epoch))
    return Admission::kDenied;

  // Verify the complete Edge offer under a separately enrolled gateway link
  // key. Merely parsing the embedded Backend MAC is NOT authorization.
  if (!macAuthorized(message, length, source, gateway_id_))
    return Admission::kDenied;
  // A credential or scope revocation during a CC310 call is not permission
  // to keep a now-unauthorized receipt in a volatile queue.
  if (!source.verifiedForGateway(gateway_id_) ||
      !source.permitsTracker(gateway_id_, tracker, key_epoch)) {
    reset();
    return Admission::kDenied;
  }

  for (size_t i = 0U; i < kCapacity; ++i) {
    const Entry& existing = entries_[i];
    if (!existing.used || existing.source_row != offer.source_row)
      continue;
    return existing.tracker == tracker &&
                   existing.key_epoch == key_epoch &&
                   existing.length == offer.receipt_size &&
                   same(existing.wire, offer.receipt, offer.receipt_size)
               ? Admission::kDuplicate : Admission::kDenied;
  }
  for (size_t i = 0U; i < kCapacity; ++i) {
    Entry& entry = entries_[i];
    if (entry.used) continue;
    entry.source_row = offer.source_row;
    entry.tracker = tracker;
    entry.key_epoch = key_epoch;
    memcpy(entry.wire, offer.receipt, offer.receipt_size);
    entry.length = static_cast<uint8_t>(offer.receipt_size);
    entry.since_ms = now_ms;
    entry.attempted = false;
    entry.used = true;
    return Admission::kQueued;
  }
  // NEVER evict a live receipt to create a fictitious success.
  return Admission::kFull;
}

bool GatewayRamRelay::select(
    uint32_t now_ms, const TxConditions& p,
    const CredentialSource& source, Candidate& output) {
  expire(now_ms);
  output = {};  // no stale candidate on any failed admission or revocation.
  if (!p.gateway_service_authorized || gateway_id_ == 0U ||
      !source.verifiedForGateway(gateway_id_)) {
    reset();
    return false;
  }
  if (!p.radio_idle || p.custody_ack_pending || p.command_pending ||
      p.live_pending || !p.regional_rf_budget_granted ||
      !p.tracker_rx_window_authorized || p.heard_tracker == 0U ||
      p.heard_age_ms > kFreshRxHintMs)
    return false;

  const Entry* candidate = nullptr;
  for (size_t i = 0U; i < kCapacity; ++i) {
    const Entry& current = entries_[i];
    if (!current.used || current.tracker != p.heard_tracker ||
        !source.permitsTracker(gateway_id_, current.tracker,
                               current.key_epoch) ||
        (current.attempted &&
         static_cast<uint32_t>(now_ms - current.last_attempt_ms) <
             kMinRetryMs))
      continue;
    // Oldest valid offer first, including across 32-bit millisecond wrap.
    if (candidate == nullptr ||
        static_cast<uint32_t>(now_ms - current.since_ms) >
        static_cast<uint32_t>(now_ms - candidate->since_ms))
      candidate = &current;
  }
  if (candidate == nullptr) return false;
  Candidate staged{};
  staged.source_row = candidate->source_row;
  staged.tracker = candidate->tracker;
  staged.length = candidate->length;
  memcpy(staged.wire, candidate->wire, candidate->length);
  output = staged;
  return true;
}

void GatewayRamRelay::markTxStarted(uint64_t source_row, uint32_t now_ms) {
  if (source_row == 0U) return;
  for (size_t i = 0U; i < kCapacity; ++i) {
    Entry& entry = entries_[i];
    if (entry.used && entry.source_row == source_row) {
      entry.attempted = true;
      entry.last_attempt_ms = now_ms;
      return;
    }
  }
}

}  // namespace orun_tlp::kq05_ram_relay
#endif  // ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
