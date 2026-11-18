#include "kq05_gateway_durable_batch_owner.h"

#include <string.h>

#include "kq05_gateway_batch_custody_crypto.h"

namespace orun_tlp {
namespace {
namespace ack = kq05_gateway_batch_ack;
namespace fmt = custody_store_product_format;

void wipe(void* memory, size_t bytes) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *p++ = 0U;
}

bool sameScope(const GatewayCustodyAckScope& a,
               const GatewayCustodyAckScope& b) {
  return a.tracker_device_id == b.tracker_device_id &&
         a.gateway_device_id == b.gateway_device_id &&
         a.key_epoch == b.key_epoch &&
         a.custody_floor == b.custody_floor &&
         a.grant_generation == b.grant_generation;
}

bool sameKey(const uint8_t (&a)[32], const uint8_t (&b)[32]) {
  volatile uint8_t difference = 0U;
  for (size_t i = 0U; i < sizeof(a); ++i)
    difference = static_cast<uint8_t>(difference | (a[i] ^ b[i]));
  return difference == 0U;
}

bool validScope(const GatewayCustodyAckScope& s, uint64_t gateway,
                uint64_t tracker, uint32_t epoch) {
  return tracker != 0U && tracker != UINT64_MAX &&
         gateway != 0U && gateway != UINT64_MAX &&
         s.tracker_device_id == tracker &&
         s.gateway_device_id == gateway && s.key_epoch == epoch &&
         epoch != UINT32_MAX && s.custody_floor != UINT32_MAX &&
         s.grant_generation != 0U &&
         s.grant_generation != UINT32_MAX;
}

bool sameHandle(const ProductCustodyStore::Handle& a,
                const ProductCustodyStore::Handle& b) {
  return a.page == b.page && a.slot == b.slot &&
         a.generation == b.generation;
}
} // namespace

Kq05GatewayDurableBatchOwner::Result
Kq05GatewayDurableBatchOwner::issue(
    uint64_t tracker_id, uint32_t key_epoch,
    const ProductCustodyStore::Handle* exact_handles, uint8_t count,
    uint8_t (&wire_out)[ack::kMaxSize], size_t& wire_length) {
  if (count == 0U || count > ack::kMaxObjects ||
      exact_handles == nullptr || gateway_id_ == 0U ||
      gateway_id_ == UINT64_MAX ||
      tracker_id == 0U || tracker_id == UINT64_MAX ||
      key_epoch == UINT32_MAX) return Result::kDenied;
  if (!store_.ready()) return Result::kNotDurable;
  if (store_.busy() || store_.hasOutcome() ||
      store_.hasMaintenanceResult()) return Result::kBusy;

  // The same Flash record cannot be counted twice, including when a caller
  // provides it via two aliases. No mutation or allocation is performed.
  for (uint8_t i = 0U; i < count; ++i) {
    for (uint8_t j = 0U; j < i; ++j)
      if (sameHandle(exact_handles[i], exact_handles[j]))
        return Result::kDenied;
  }

  GatewayCustodyAckScope grant{};
  uint8_t key[32]{};
  const bool authorized =
      grants_.activeGrant(tracker_id, key_epoch, grant, key) &&
      validScope(grant, gateway_id_, tracker_id, key_epoch);
  if (!authorized) {
    wipe(key, sizeof(key));
    return Result::kDenied;
  }

  uint8_t frames[ack::kMaxObjects][fmt::kMaxObjectSize]{};
  Kq05BatchExactView views[ack::kMaxObjects]{};
  size_t frame_lengths[ack::kMaxObjects]{};
  bool found = true;
  for (uint8_t i = 0U; i < count; ++i) {
    size_t n = 0U;
    if (store_.readConfirmedDurable(
            exact_handles[i], frames[i], n) !=
            ProductCustodyStore::Lookup::kFound ||
        (n != fmt::kPeriodicSize && n != fmt::kEventSize)) {
      found = false;
      break;
    }
    frame_lengths[i] = n;
    views[i].bytes = frames[i];
    views[i].size = n;
  }

  uint8_t candidate[ack::kMaxSize]{};
  size_t candidate_length = 0U;
  const bool signed_ok = found && store_.ready() &&
      !store_.busy() && !store_.hasOutcome() &&
      !store_.hasMaintenanceResult() &&
      Kq05GatewayBatchCustodyCrypto::signAfterDurableReadback(
          grant, key, views, count, candidate, candidate_length);

  // This second grant observation is essential: a revoked/rotated scope or
  // key (even if its public fields were not changed correctly) must never
  // publish an ACK. Only the physical Gateway grant provider can mint it.
  GatewayCustodyAckScope latest{};
  uint8_t latest_key[32]{};
  const bool still_granted = signed_ok &&
      grants_.activeGrant(tracker_id, key_epoch, latest, latest_key) &&
      sameScope(latest, grant) && sameKey(latest_key, key);
  wipe(latest_key, sizeof(latest_key));
  wipe(key, sizeof(key));

  // Reject if any accepted Flash record changed between the initial durable
  // readback and MAC computation, even if this owner is accidentally called
  // without the required single-loop serialization by its composition root.
  uint8_t again[fmt::kMaxObjectSize]{};
  bool unchanged = still_granted && store_.ready() && !store_.busy() &&
                   !store_.hasOutcome() && !store_.hasMaintenanceResult();
  for (uint8_t i = 0U; unchanged && i < count; ++i) {
    size_t n = 0U;
    unchanged =
        store_.readConfirmedDurable(exact_handles[i], again, n) ==
            ProductCustodyStore::Lookup::kFound &&
        n == frame_lengths[i] &&
        memcmp(again, frames[i], n) == 0;
  }
  wipe(again, sizeof(again));
  wipe(frames, sizeof(frames));
  wipe(views, sizeof(views));
  wipe(frame_lengths, sizeof(frame_lengths));

  if (!unchanged || !signed_ok || candidate_length > sizeof(wire_out)) {
    wipe(candidate, sizeof(candidate));
    return found ? Result::kDenied : Result::kNotDurable;
  }
  memcpy(wire_out, candidate, candidate_length);
  wire_length = candidate_length;
  wipe(candidate, sizeof(candidate));
  return Result::kReady;
}

} // namespace orun_tlp
