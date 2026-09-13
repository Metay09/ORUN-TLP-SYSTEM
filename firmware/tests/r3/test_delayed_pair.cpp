#include "../m3/gnss_test_support.h"

// Uses only pre-R3 public APIs, so the same regression can run against 36ed21c.
void delayedPair(bool dop_first, uint32_t age) {
  GnssManager manager;
  boot(manager, UINT32_MAX - 3000);
  emitPvt(manager, pvt(1000)); emitDop(manager, 1000);
  if (dop_first) emitDop(manager, 2000);
  else emitPvt(manager, pvt(2000));
  test_now += age;
  if (dop_first) emitPvt(manager, pvt(2000));
  else emitDop(manager, 2000);
  GnssFix fix{};
  assert(!manager.takeFreshFixForTransmission(&fix));
}

int main() {
  delayedPair(false, gnss_config::kFreshFixMaxAgeMs + 1);
  delayedPair(true, gnss_config::kFreshFixMaxAgeMs + 1);
  delayedPair(false, gnss_config::kFreshFixMaxAgeMs);
  delayedPair(true, gnss_config::kFreshFixMaxAgeMs);
  puts("R3 delayed PVT/DOP regression checks: PASS");
}
