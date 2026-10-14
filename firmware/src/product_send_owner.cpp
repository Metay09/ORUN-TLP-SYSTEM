#include "product_send_owner.h"

#include <string.h>

namespace orun_tlp {
namespace {

bool validExact(const ProductSendExact& exact,
                const ProductSendIdentity& selected) {
  return selected.valid() &&
         sameProductSendIdentity(exact.identity, selected) &&
         exact.size == sizeof(exact.bytes);
}

bool identical(const ProductSendExact& a, const ProductSendExact& b) {
  return a.size == b.size &&
         sameProductSendIdentity(a.identity, b.identity) &&
         memcmp(a.bytes, b.bytes, a.size) == 0;
}

bool due(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace

void ProductSendOwner::fail() {
  state_ = State::kFault;
  exact_ = ProductSendExact{};
  ++diagnostics_.failures;
}

bool ProductSendOwner::loadDurable(const ProductSendExact* expected) {
  ProductSendExact found{};
  if (port_.cached(selected_, found) != ProductSendRead::kFound ||
      !validExact(found, selected_) ||
      (expected != nullptr && !identical(found, *expected))) {
    fail();
    return false;
  }
  // Without lifetime authority, even a byte-perfect cache is NOT RF eligible.
  if (!port_.currentCredentialOwns(found)) {
    fail();
    return false;
  }
  exact_ = found;
  state_ = State::kRetry;
  return true;
}

void ProductSendOwner::scheduleRetry(uint32_t now_ms) {
  // 30 s, 60 s, 120 s, 240 s, 480 s, max 15 min.
  uint32_t delay_ms = 30000U << (retry_level_ > 4U ? 4U : retry_level_);
  if (delay_ms > 900000U) delay_ms = 900000U;
  if (retry_level_ < 5U) ++retry_level_;
  next_retry_ms_ = now_ms + delay_ms;
  retry_scheduled_ = true;
}

void ProductSendOwner::poll(uint32_t now_ms) {
  if (state_ == State::kFault) return;

  if (state_ == State::kAwaitCache) {
    ProductSendIdentity completed{};
    bool durable = false;
    if (!port_.takeCacheResult(completed, durable)) return;
    if (!durable || !sameProductSendIdentity(completed, selected_)) {
      fail();
      return;
    }
    if (!loadDurable(&exact_)) return;
    ++diagnostics_.cache_committed;
  }

  if (!port_.securityReady()) {
    ++diagnostics_.blocked_security;
    return;
  }

  if (state_ == State::kIdle) {
    if (scan_scheduled_ && !due(now_ms, next_scan_ms_)) return;
    ProductSendPeriodic candidate{};
    const ProductSendRead read = port_.oldestPeriodic(candidate);
    if (read == ProductSendRead::kNone) {
      // No observations: do not scan the 128 KiB store in every loop.
      scan_scheduled_ = true;
      next_scan_ms_ = now_ms + 30000U;
      return;
    }
    if (read != ProductSendRead::kFound || !candidate.identity.valid()) {
      fail();
      return;
    }
    selected_ = candidate.identity;
    ProductSendExact existing{};
    const ProductSendRead cached = port_.cached(selected_, existing);
    if (cached == ProductSendRead::kFound) {
      if (!loadDurable(nullptr)) return;
      ++diagnostics_.cached_recovered;
    } else if (cached == ProductSendRead::kNone) {
      ProductSendExact sealed{};
      const ProductSendSeal seal = port_.sealOnce(candidate, sealed);
      if (seal == ProductSendSeal::kUnavailable) return;
      if (seal != ProductSendSeal::kOk ||
          !validExact(sealed, selected_)) {
        fail();
        return;
      }
      ++diagnostics_.sealed;
      exact_ = sealed;
      switch (port_.submitCache(sealed)) {
        case ProductSendCache::kStarted:
          state_ = State::kAwaitCache;
          return;
        case ProductSendCache::kAlreadyDurable:
          if (!loadDurable(&sealed)) return;
          break;
        case ProductSendCache::kUnavailable:
        case ProductSendCache::kNoCapacity:
        case ProductSendCache::kRejected:
          // Never re-seal on a timer; no TX from an uncommitted frame.
          fail();
          return;
      }
    } else {
      fail();
      return;
    }
  }

  if (state_ != State::kRetry ||
      (retry_scheduled_ && !due(now_ms, next_retry_ms_))) return;
  // Do not scan flash while the radio owner cannot accept a TX.
  if (!port_.radioReady()) return;
  // Revalidate backing ownership and exact bytes on EVERY retry. A release,
  // stale key lifetime, torn read, or changed cache permanently blocks TX.
  ProductSendPeriodic retained{};
  if (port_.oldestPeriodic(retained) != ProductSendRead::kFound ||
      !sameProductSendIdentity(retained.identity, selected_) ||
      !loadDurable(&exact_)) {
    if (state_ != State::kFault) fail();
    return;
  }
  if (port_.transmitExact(exact_.bytes, exact_.size))
    ++diagnostics_.tx_started;
  else
    ++diagnostics_.tx_refused;
  // A TX start, TX_DONE or timeout is NOT custody and NEVER releases a row.
  scheduleRetry(now_ms);
}

}  // namespace orun_tlp
