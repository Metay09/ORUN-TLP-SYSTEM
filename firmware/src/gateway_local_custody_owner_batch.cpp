#include "gateway_local_custody_owner.h"

#include <string.h>

#include "kq05_gateway_batch_custody_crypto.h"

namespace orun_tlp {
namespace {
void wipeBatch(void* data, size_t n) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(data);
  while (n--) *p++ = 0U;
}
}  // namespace

bool GatewayLocalCustodyOwner::signDurableBatch(
    const ProductCustodyStore::Handle* handles, uint8_t count,
    const GatewayCustodyAckScope& scope, const uint8_t (&scoped_key)[32],
    uint8_t (&wire)[kq05_gateway_batch_ack::kMaxSize],
    size_t& written, bool* stale_single) {
  // Reuse ProductCustodyStore, not a second intake/flash state machine.
  // No signed ACK while its single owner has an incomplete write/outcome.
  if (handles == nullptr || count == 0U ||
      count > kq05_gateway_batch_ack::kMaxObjects ||
      waiting_flash_ || ack_ready_ || !store_.ready() ||
      store_.busy() || store_.hasOutcome()) return false;

  uint8_t exact[kq05_gateway_batch_ack::kMaxObjects]
               [custody_store_product_format::kMaxObjectSize]{};
  Kq05BatchExactView views[kq05_gateway_batch_ack::kMaxObjects]{};
  bool proven = true;
  for (uint8_t i = 0U; i < count; ++i) {
    // A single durable handle may not be counted as two receipts.
    for (uint8_t j = 0U; j < i; ++j)
      if (handles[j].page == handles[i].page &&
          handles[j].slot == handles[i].slot &&
          handles[j].generation == handles[i].generation)
        proven = false;
    if (!proven) break;

    size_t length = 0U;
    const auto lookup = store_.readConfirmedDurable(handles[i], exact[i], length);
    if (stale_single && count == 1 && lookup == ProductCustodyStore::Lookup::kNotHeld)
      *stale_single = true;
    if (lookup != ProductCustodyStore::Lookup::kFound ||
        (length != 84U && length != 100U)) {
      proven = false;
      break;
    }
    views[i].bytes = exact[i];
    views[i].size = length;
  }

  bool signed_ok = false;
  if (proven) {
    // The existing private crypto primitive additionally proves all
    // protected shapes, tracker/epoch, distinct full SHA-256 digests,
    // sorted 8-byte hints and a domain-separated HMAC.
    uint8_t candidate[kq05_gateway_batch_ack::kMaxSize]{};
    size_t produced = 0U;
    signed_ok = Kq05GatewayBatchCustodyCrypto::signAfterDurableReadback(
        scope, scoped_key, views, count, candidate, produced);
    if (signed_ok) {
      memcpy(wire, candidate, produced);
      written = produced;
    }
    wipeBatch(candidate, sizeof(candidate));
  }
  wipeBatch(exact, sizeof(exact));
  return signed_ok;
}

}  // namespace orun_tlp
