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

void ProductSendOwner::scheduleHalfRetry(uint32_t now_ms) {
  // Exactly ONE supplemental retry after half the *configured* report
  // interval. The next attempt thereafter requires a new committed report
  // period; never spin at 30 s / 15 min while out of coverage.
  const uint32_t period_ms = port_.reportPeriodMs();
  if (period_ms < 2000U || period_ms > 0x7FFFFFFEU) {
    // Missing/invalid cadence is no license to invent periodic RF retries.
    half_retry_consumed_ = true;
    retry_scheduled_ = false;
    state_ = State::kWaitReport;
    return;
  }
  next_retry_ms_ = now_ms + period_ms / 2U;
  retry_scheduled_ = true;
  state_ = State::kRetry;
}

void ProductSendOwner::onReportCommitted() {
  // A stored new logical record provides a bounded contact opportunity.
  // Re-probe using the *same* cached object; do not mint a fresh ciphertext
  // every hour and exhaust the four-object custody cache during an outage.
  if (state_ == State::kWaitReport) {
    retry_scheduled_ = false;
    state_ = State::kRetry;
  }
}

void ProductSendOwner::onCustodyReleaseCommitted(
    const ProductSendIdentity& identity, uint32_t now_ms) {
  if (state_ == State::kFault || !identity.valid() ||
      !sameProductSendIdentity(identity, selected_) ||
      (state_ != State::kRetry && state_ != State::kWaitReport))
    return;
  // This notification is not itself an ACK. A future reviewed caller must
  // authenticate and validate exact-object custody, then durably release
  // the selected record BEFORE invoking this method. We never release here.
  selected_ = ProductSendIdentity{};
  exact_ = ProductSendExact{};
  state_ = State::kIdle;
  half_retry_consumed_ = false;
  retry_scheduled_ = false;
  scan_scheduled_ = true;
  next_scan_ms_ = now_ms + kBacklogPaceMs;
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
      if (!loadDurable(&existing)) return;
      ++diagnostics_.cached_recovered;
    } else if (cached == ProductSendRead::kNone) {
      ProductSendExact sealed{};
      const ProductSendSeal seal = port_.sealOnce(candidate, sealed);
      if (seal == ProductSendSeal::kUnavailable) {
        // Counter reservations may be temporarily busy. Avoid repeatedly
        // scanning the entire product region and invoking the crypto owner
        // in a tight loop while the flash/security gate is unavailable.
        scan_scheduled_ = true;
        next_scan_ms_ = now_ms + 60000U;
        return;
      }
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
  // TX start/TX_DONE/timeout is NOT custody. After the single half-period
  // retry, quiet until an actual completed report or authenticated handoff.
  if (!half_retry_consumed_ && !retry_scheduled_) {
    scheduleHalfRetry(now_ms);
  } else {
    half_retry_consumed_ = true;
    retry_scheduled_ = false;
    state_ = State::kWaitReport;
  }
}

}  // namespace orun_tlp
