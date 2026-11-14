#include <assert.h>
#include <stdint.h>

#include "rf_duty_budget.h"

using orun_tlp::RfDutyBudget;
using Frame = RfDutyBudget::Frame;
using Decision = RfDutyBudget::Decision;

static void testColdBootAndSharedLimit() {
  RfDutyBudget b;
  b.begin(100U);
  const uint32_t now = 100U;
  // The device starts with an immediately available fresh budget.
  for (int i = 0; i < 7; ++i) {
    auto kind = static_cast<Frame>(i);
    assert(b.reserve(now, 45000U, kind) == Decision::kAllowed);
  }
  assert(b.reserve(now, 45000U, Frame::kCustodyAck) == Decision::kAllowed);
  assert(b.usedMs() == 360000U);
  assert(b.reserve(now, 1U, Frame::kProductSecure) == Decision::kExhausted);
}

static void testSlidingWindowAndConservativeExpiry() {
  RfDutyBudget b;
  b.begin(0U);
  assert(b.reserve(0U, 360000U, Frame::kProductSecure) ==
         Decision::kAllowed);
  assert(b.reserve(3599999U, 1U, Frame::kCustodyAck) ==
         Decision::kExhausted);
  // A one-minute conservative guard interval also protects bucket edges.
  assert(b.reserve(3600000U, 1U, Frame::kCustodyAck) ==
         Decision::kExhausted);
  assert(b.reserve(3660000U, 1U, Frame::kCustodyAck) ==
         Decision::kAllowed);
}

static void testClockRegressionsFailClosed() {
  RfDutyBudget b;
  b.begin(400U);
  assert(b.reserve(399U, 1U, Frame::kTest) == Decision::kClockFault);
  assert(b.faulted());
  assert(b.reserve(3600400U, 1U, Frame::kTest) == Decision::kClockFault);
}

static void testWrappedMillisClock() {
  RfDutyBudget b;
  b.begin(0xFFFFF000UL);
  assert(b.reserve(3595904U, 2300U, Frame::kHistorySecure) ==
         Decision::kAllowed);
}

static void testInvalidArguments() {
  RfDutyBudget b;
  b.begin(0U);
  assert(b.reserve(3600000U, 100U, static_cast<Frame>(42U)) ==
         Decision::kBadFrame);
  assert(b.reserve(3600000U, 0U, Frame::kTest) ==
         Decision::kBadAirtime);
  assert(b.reserve(3600000U, 360001U, Frame::kTest) ==
         Decision::kBadAirtime);
  assert(b.reserve(3600000U, 100U, Frame::kTest) ==
         Decision::kAllowed);
}

static void testRebootGetsFreshBudgetInDevelopment() {
  RfDutyBudget before;
  before.begin(0U);
  assert(before.reserve(0U, 360000U, Frame::kPosition) ==
         Decision::kAllowed);
  assert(before.reserve(100U, 1U, Frame::kPosition) ==
         Decision::kExhausted);

  // Explicit development policy: reboot forgets prior budget usage.
  // This must not be mistaken for a rolling-hour regulatory guarantee.
  RfDutyBudget after;
  after.begin(0U);
  assert(after.reserve(0U, 1U, Frame::kCustodyAck) ==
         Decision::kAllowed);
  assert(after.usedMs() == 1U);
}

int main() {
  testColdBootAndSharedLimit();
  testSlidingWindowAndConservativeExpiry();
  testClockRegressionsFailClosed();
  testWrappedMillisClock();
  testInvalidArguments();
  testRebootGetsFreshBudgetInDevelopment();
  return 0;
}
