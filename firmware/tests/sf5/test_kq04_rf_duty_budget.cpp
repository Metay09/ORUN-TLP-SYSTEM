#include <assert.h>
#include <stdint.h>

#include "rf_duty_budget.h"

using orun_tlp::RfDutyBudget;
using Frame = RfDutyBudget::Frame;
using Decision = RfDutyBudget::Decision;

static void testColdBootAndSharedLimit() {
  RfDutyBudget b;
  b.begin(100U);
  assert(b.reserve(100U, 2000U, Frame::kPosition) == Decision::kBootHold);
  assert(b.reserve(3600099U, 2000U, Frame::kPosition) == Decision::kBootHold);
  uint32_t now = 3600100U;
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
  assert(b.reserve(3600000U, 360000U, Frame::kProductSecure) ==
         Decision::kAllowed);
  assert(b.reserve(7199999U, 1U, Frame::kCustodyAck) ==
         Decision::kExhausted);
  // Minute 60 is retained through minute 120. At minute 121 it expires.
  assert(b.reserve(7260000U, 1U, Frame::kCustodyAck) ==
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

static void testRebootCannotImmediatelyGrantBudget() {
  RfDutyBudget before;
  before.begin(0U);
  assert(before.reserve(3600000U, 360000U, Frame::kPosition) ==
         Decision::kAllowed);
  RfDutyBudget after;
  after.begin(0U);
  assert(after.reserve(0U, 1U, Frame::kCustodyAck) ==
         Decision::kBootHold);
}

int main() {
  testColdBootAndSharedLimit();
  testSlidingWindowAndConservativeExpiry();
  testClockRegressionsFailClosed();
  testWrappedMillisClock();
  testInvalidArguments();
  testRebootCannotImmediatelyGrantBudget();
  return 0;
}
