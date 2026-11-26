#include <assert.h>
#include <stdio.h>

#include "product_report_timing.h"
#include "product_period_builder.h"

using namespace orun_tlp;

int main() {
  const uint32_t intervals[] = {60000U, 900000U};

  for (const uint32_t interval : intervals) {
    const uint32_t start = 100U;

    assert(!productReportDue(false, start, start + interval,
                             interval, true));

    // First fix: minimum real duration, no fabricated second.
    assert(!productReportDue(true, start, start + 999U,
                             interval, true));
    assert(productReportDue(true, start, start + 1000U,
                            interval, true));
    assert(productReportDue(true, start, start + 3000U,
                            interval, true));

    // GNSS absent or never reporting a timeout.
    assert(!productReportDue(true, start, start + interval - 1U,
                             interval, false));
    assert(productReportDue(true, start, start + interval,
                            interval, false));

    assert(productReportRemainingMs(
        true, start, start + interval - 1U, interval) == 1U);

    assert(productReportRemainingMs(
        true, start, start + interval, interval) == 0U);

    assert(productReportRemainingMs(
        true, start, start + interval + 1U, interval) == 0U);

    assert(productReportRemainingMs(
        true, start, start + interval + 1000U, interval) == 0U);

    // Pending first/new fix reserves live access.
    assert(productReportRemainingMs(
        true, start, start + 1000U, interval, true) == 0U);

    // Re-anchor to actual emission: no invented catch-up reports.
    const uint32_t next = start + interval + 1000U;
    assert(!productReportDue(true, next, next, interval, false));
    assert(productReportRemainingMs(
        true, next, next, interval) == interval);

    // A genuinely encoded PERIODIC without coordinates.
    PeriodicRecordInputs input{};
    input.record_incarnation = 1U;
    input.record_sequence = 1U;
    input.period_duration_seconds = interval / 1000U;

    tlp::PeriodicObservationPlaintext record{};
    assert(buildPeriodicPlaintext(input, &record) ==
           tlp::ProductSecureCodecResult::kOk);

    assert((record.record_flags &
            tlp::kPeriodicLocationValid) == 0U);

    printf("ORUN-301 interval=%lu first-fix/no-fix/overdue PASS\n",
           (unsigned long)interval);
  }

  const uint32_t start = UINT32_MAX - 500U;
  const uint32_t now = start + 1000U;

  assert(productReportDue(true, start, now, 60000U, true));

  assert(productReportRemainingMs(
      true, start, now, 60000U) == 59000U);

  puts("ORUN-301 clock wrap PASS");
  return 0;
}
