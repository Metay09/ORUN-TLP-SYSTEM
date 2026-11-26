// Actual ProductRecordStore with synchronous/asynchronous fake NOR.
// This is not the complete setup()/loop() composition test.
#define main prior_product_store_main
#include "test_sf5f_product_record_store.cpp"
#undef main

#include "product_report_timing.h"

static void verifyRecord(
    const ProductRecordStore& records,
    uint32_t sequence, bool location_valid,
    uint32_t duration) {

  osf::RecordIdentity identity{};
  identity.incarnation = records.store().incarnation();
  identity.sequence = sequence;

  ObservationStore::Record row{};

  assert(records.store().lookup(identity, row) ==
         ObservationStore::LookupResult::kFound);

  assert(!row.released);

  tlp::PeriodicObservationPlaintext decoded{};

  assert(tlp::decodePeriodicPlaintext(
      row.payload, row.payload_size, &decoded) ==
      tlp::ProductSecureCodecResult::kOk);

  assert(decoded.record_sequence == sequence);

  assert(decoded.period_duration_seconds == duration);

  assert(bool(decoded.record_flags &
              tlp::kPeriodicLocationValid) == location_valid);

  if (!location_valid) {
    assert(decoded.latitude_e7 == 0);
    assert(decoded.longitude_e7 == 0);
  }
}

static void testCadence(bool async, uint32_t interval) {
  FakeFlash flash;
  flash.async = async;

  FixedIncarnation source;
  ProductRecordStore records(flash, kPages, &source);

  records.begin(kDevice, kTracker);
  assert(records.mounted());

  RequestedConfig requested(
      true, false, RequestedLocationSource::kGnss);

  const CapabilityPresence presence[] = {
      CapabilityPresence::kUnknown,
      CapabilityPresence::kAbsent,
      CapabilityPresence::kPresent
  };

  for (auto state : presence) {
    const CapabilitySnapshot capabilities(
        CapabilityState(true, state,
                        CapabilityHealth::kUnavailable));

    const auto effective =
        resolveRequestedConfig(requested, capabilities);

    assert(effective.tracking.state == ServiceState::kBlocked);

    // GNSS blocked must not block product reporting.
    assert(productReportingEnabled(true, requested));
    assert(!productReportingEnabled(false, requested));
  }

  RequestedConfig disabled(
      false, false, RequestedLocationSource::kGnss);

  assert(!productReportingEnabled(true, disabled));

  uint32_t start = 100U;

  auto report = [&](uint32_t now, bool fix,
                    uint32_t sequence) {
    assert(productReportDue(
        true, start, now, interval, fix));

    auto input = period(sequence);

    input.period_duration_seconds =
        (now - start) / 1000U;

    if (!fix)
      input.location = PeriodicLocationInput{};

    assert(records.submitPeriodic(input) ==
           ProductRecordStore::Submit::kAccepted);

    ProductRecordStore::Outcome outcome{};
    bool done = false;

    for (unsigned i = 0; i < 20000U; ++i) {
      records.poll();
      if (records.takeOutcome(outcome)) {
        done = true;
        break;
      }
    }

    assert(done && outcome.stored);
    assert(outcome.sequence == sequence);

    verifyRecord(
        records, sequence, fix,
        input.period_duration_seconds);

    start = now;
  };

  // First accepted fix does not wait for a full interval.
  report(start + 3000U, true, 1U);

  // No GNSS fix, no timeout callback needed.
  report(start + interval, false, 2U);
  report(start + interval, false, 3U);

  // Recovery: fresh fix admitted before next full interval.
  report(start + 1000U, true, 4U);

  assert(records.diagnostics().stored == 4U);
  assert(records.diagnostics().failed == 0U);

  uint32_t retained = 0U;
  assert(records.store().retainedCount(retained));
  assert(retained == 4U);

  flash.async = false;

  ProductRecordStore reboot(flash, kPages, &source);
  reboot.begin(kDevice, kTracker);

  assert(reboot.mounted());
  verifyRecord(reboot, 1U, true, 3U);
  verifyRecord(reboot, 2U, false, interval / 1000U);
  verifyRecord(reboot, 3U, false, interval / 1000U);
  verifyRecord(reboot, 4U, true, 1U);

  printf("ORUN-301 durable first/no-fix/recovery interval=%lu async=%u PASS\n",
         (unsigned long)interval, unsigned(async));
}

int main() {
  for (uint32_t interval : {60000U, 900000U}) {
    testCadence(false, interval);
    testCadence(true, interval);
  }
  return 0;
}
