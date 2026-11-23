#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "sf5l0_bench.h"
#include "gateway_radio_timing.h"
#include "tlp_v2_product_secure.h"

using namespace orun_tlp;

int main() {
  const uint8_t live_size =
      static_cast<uint8_t>(tlp::kPeriodicFrameSize);
  const uint8_t event_size =
      static_cast<uint8_t>(tlp::kEventFrameSize);

  static_assert(tlp::kPeriodicFrameSize == 100U,
                "Periodic frame changed");
  static_assert(tlp::kEventFrameSize == 84U,
                "Event frame changed");

  assert(profileAirtimeUs(event_size) <
         profileAirtimeUs(live_size));

  // Tight budget: enough for one initial live packet,
  // but not for an old packet plus an imminent live report.
  Sf5l0DrainPacer::Config cfg;
  cfg.burst_ms = 2300U;
  cfg.duty_ppm = 60000U;

  Sf5l0DrainPacer one_minute(cfg);
  one_minute.begin(0U);

  assert(one_minute.hasBatchAirtime(0U, live_size));

  // Fresh report imminent: backlog must not transmit.
  assert(!one_minute.hasBacklogAirtime(
      0U, live_size, 0U));
  assert(!one_minute.hasBacklogAirtime(
      0U, live_size, 10000U));
  assert(!one_minute.hasBacklogAirtime(
      0U, live_size, 20000U));

  // With 60 seconds until the next report,
  // refill can restore the live packet allowance.
  assert(one_minute.hasBacklogAirtime(
      0U, live_size, 60000U));

  // Charge a real-size packet and respect RX time.
  one_minute.chargeBatchTx(0U, live_size);
  assert(!one_minute.hasBatchAirtime(
      1000U, live_size));
  assert(!one_minute.hasBatchAirtime(
      11000U, live_size));

  assert(one_minute.hasBatchAirtime(
      60000U, live_size));
  assert(!one_minute.hasBacklogAirtime(
      60000U, live_size, 18000U));

  // 15-minute cadence should allow surplus backlog
  // when the current budget is sufficient.
  Sf5l0DrainPacer fifteen_minutes(cfg);
  fifteen_minutes.begin(0U);
  assert(fifteen_minutes.hasBacklogAirtime(
      0U, live_size, 900000U));
  assert(!fifteen_minutes.hasBacklogAirtime(
      0U, live_size, 0U));

  // Critical EVENT uses its actual shorter airtime.
  Sf5l0DrainPacer critical(cfg);
  critical.begin(0U);
  assert(critical.hasBatchAirtime(0U, event_size));
  critical.chargeBatchTx(0U, event_size);
  assert(!critical.hasBatchAirtime(
      1000U, live_size));

  // Millisecond counter wrap.
  Sf5l0DrainPacer rollover(cfg);
  const uint32_t start = 0xFFFFFF00U;
  rollover.begin(start);
  rollover.chargeBatchTx(start, live_size);
  assert(rollover.hasBatchAirtime(
      start + 60000U, live_size));

  printf("LIVE RESERVE: PASS\n");
  printf("Periodic 100B airtime: %lu us\n",
         static_cast<unsigned long>(
             profileAirtimeUs(live_size)));
  printf("Critical 84B airtime: %lu us\n",
         static_cast<unsigned long>(
             profileAirtimeUs(event_size)));
  puts("60s / 900s / low budget / RX / critical / wrap: PASS");
  return 0;
}
