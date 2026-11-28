#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "sf5l0_bench.h"
#include "gateway_radio_timing.h"
#include "tlp_v2_product_secure.h"

using namespace orun_tlp;

constexpr uint8_t live = tlp::kPeriodicFrameSize;
constexpr uint8_t event = tlp::kEventFrameSize;

static bool backlogAllowed(Sf5l0DrainPacer& pacer, RfDutyBudget& budget,
                           uint32_t now, uint32_t until_live) {
  return pacer.hasBatchAirtime(now, live) &&
         budget.available(now, live) &&
         pacer.hasBacklogAirtime(now, live, until_live) &&
         budget.availableBeforeLive(now, live, until_live, live);
}

int main() {
  static_assert(live == 100U, "Periodic frame changed");
  static_assert(event == 84U, "Event frame changed");
  const uint32_t cost = RfDutyBudget::costMs(live);
  const uint32_t gap = cost + radio_config::kWindowedRxAfterTxMs;
  assert(profileAirtimeUs(event) < profileAirtimeUs(live));

  Sf5l0DrainPacer pacer;
  RfDutyBudget budget;
  pacer.begin(0U);

  // The pacer owns RX spacing, the ledger owns airtime credit.
  assert(!backlogAllowed(pacer, budget, 0U, 0U));
  assert(!backlogAllowed(pacer, budget, 0U, 10000U));
  assert(!backlogAllowed(pacer, budget, 0U, gap + 200U));
  assert(backlogAllowed(pacer, budget, 0U, gap + 201U));
  assert(backlogAllowed(pacer, budget, 0U, 20000U));
  assert(backlogAllowed(pacer, budget, 0U, 60000U));
  assert(backlogAllowed(pacer, budget, 0U, 900000U));

  assert(budget.reserve(0U, live));
  pacer.chargeBatchTx(0U, live);
  assert(!budget.available(cost - 1U, live));
  budget.finish(cost);
  assert(!pacer.hasBatchAirtime(10000U, live));
  assert(!pacer.hasBatchAirtime(gap - 1U, live));
  assert(pacer.hasBatchAirtime(gap, live));
  assert(backlogAllowed(pacer, budget, 60000U, 18000U));

  // Consume the real rolling-hour credit until only one live fits.
  RfDutyBudget tight;
  Sf5l0DrainPacer idle;
  idle.begin(0U);
  for (uint32_t i = 0U; i < 161U; ++i) {
    assert(tight.reserve(i * cost, live));
    tight.finish((i + 1U) * cost);
  }
  const uint32_t now = 400000U;
  assert(tight.available(now, live));
  assert(tight.usedMs() == 161U * cost);
  assert(!backlogAllowed(idle, tight, now, 20000U));
  assert(!backlogAllowed(idle, tight, now, 60000U));
  assert(!backlogAllowed(idle, tight, now, 900000U));
  // Live itself remains admissible.
  assert(tight.reserve(now, live));
  tight.finish(now + cost);
  assert(!tight.available(now + cost, live));
  // Credit expires with real elapsed time, not a second token bucket.
  assert(backlogAllowed(idle, tight, 3663000U, 60000U));

  RfDutyBudget forecast;
  for (uint32_t i = 0U; i < 161U; ++i) {
    assert(forecast.reserve(i * cost, live));
    forecast.finish((i + 1U) * cost);
  }
  // First retained minute expires before the next live report.
  // A TX beginning just before the minute boundary extends retention
  // beyond that boundary. The first full minute alone is insufficient.
  assert(!backlogAllowed(idle, forecast, 3600000U, 60000U));
  assert(backlogAllowed(idle, forecast, 3603000U, 60000U));

  RfDutyBudget critical_budget;
  Sf5l0DrainPacer critical;
  critical.begin(0U);
  assert(critical.hasBatchAirtime(0U, event));
  assert(critical_budget.reserve(0U, event));
  critical.chargeBatchTx(0U, event);
  critical_budget.finish(RfDutyBudget::costMs(event));
  assert(!critical.hasBatchAirtime(1000U, live));
  assert(critical_budget.usedMs() == RfDutyBudget::costMs(event));

  RfDutyBudget wrap_budget;
  Sf5l0DrainPacer wrap;
  const uint32_t start = 0xFFFFFF00U;
  wrap.begin(start);
  assert(wrap_budget.reserve(start, live));
  wrap.chargeBatchTx(start, live);
  wrap_budget.finish(start + cost);
  assert(!wrap.hasBatchAirtime(start + gap - 1U, live));
  assert(backlogAllowed(wrap, wrap_budget, start + 60000U, 60000U));

  puts("ORUN-305 LIVE RESERVE real pacer + shared RF ledger: PASS");
  puts("60s / 900s / low budget / RX deadline / expiry / EVENT / wrap: PASS");
  return 0;
}
