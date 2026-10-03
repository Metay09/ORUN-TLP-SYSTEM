#include <assert.h>
#include <stdint.h>

#include "location_owner.h"
#include "monotonic_time.h"

using namespace orun_tlp;

int main() {
  LocationOwner owner;
  AcceptedLocation out{};
  out.latitude_e7 = 123;
  assert(!owner.hasLocation());
  assert(!owner.latest(&out));
  assert(out.latitude_e7 == 123);
  assert(!owner.latest(nullptr));

  AcceptedLocation first{};
  first.observed_monotonic_ms = 123456789ULL;
  first.utc_epoch_seconds = 1700000000UL;
  first.latitude_e7 = 0;
  first.longitude_e7 = 0;
  first.altitude_mm = 3210;
  first.source = LocationSource::kGnss;
  first.altitude_valid = true;
  first.utc_valid = true;
  assert(owner.accept(first));
  assert(owner.hasLocation());
  assert(owner.latest(&out));
  assert(out.latitude_e7 == 0);
  assert(out.longitude_e7 == 0);
  assert(out.altitude_mm == 3210);
  assert(out.source == LocationSource::kGnss);
  assert(out.altitude_valid);
  assert(out.utc_valid);

  AcceptedLocation invalid = first;
  invalid.source = LocationSource::kUnknown;
  invalid.latitude_e7 = 111;
  assert(!owner.accept(invalid));
  assert(owner.latest(&out));
  assert(out.latitude_e7 == first.latitude_e7);

  invalid = first;
  invalid.latitude_e7 = 900000001;
  assert(!owner.accept(invalid));
  assert(owner.latest(&out));
  assert(out.latitude_e7 == first.latitude_e7);

  invalid = first;
  invalid.longitude_e7 = -1800000001;
  assert(!owner.accept(invalid));
  assert(owner.latest(&out));
  assert(out.longitude_e7 == first.longitude_e7);

  AcceptedLocation second = first;
  second.observed_monotonic_ms += 5000;
  second.latitude_e7 = 376104262;
  second.longitude_e7 = 280545800;
  second.utc_epoch_seconds = 0;
  second.utc_valid = false;
  assert(owner.accept(second));
  assert(owner.latest(&out));
  assert(out.latitude_e7 == second.latitude_e7);
  assert(out.longitude_e7 == second.longitude_e7);
  assert(out.observed_monotonic_ms == second.observed_monotonic_ms);
  assert(!out.utc_valid);

  constexpr uint64_t kAfterFirstWrap = (1ULL << 32) + 0x10ULL;
  constexpr uint32_t kCapturedBeforeWrap = 0xFFFFFFF0UL;
  static_assert(
      extendRecentMonotonicMs(kAfterFirstWrap, kCapturedBeforeWrap) ==
          0x00000000FFFFFFF0ULL,
      "recent capture must extend across rollover");

  monotonic::TickMillis clock;
  const uint64_t before_wrap = clock.update64(UINT32_MAX - 2U, 1024U);
  const uint64_t after_wrap = clock.update64(3U, 1024U);
  assert(after_wrap > before_wrap);
  assert(after_wrap - before_wrap >= 5U);
  assert(after_wrap - before_wrap <= 6U);

  monotonic::TickMillis compatibility_clock;
  assert(compatibility_clock.update(1024U, 1024U) == 1000U);
  assert(compatibility_clock.update64(2048U, 1024U) == 2000U);

  return 0;
}
