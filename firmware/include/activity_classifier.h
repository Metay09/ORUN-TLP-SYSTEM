#pragma once

#include <stdint.h>

namespace orun_tlp {

// First active/inactive window classifier (owner decision 2026-10-11:
// "use other people's published measurements"). RESEARCH-DERIVED and NOT
// validated on the ORUN collar, its mounting or our animals; it must not be
// presented as validated behaviour accuracy (AGENTS.md, Accelerometer).
//
// Source: Diosdado et al. 2015, Animal Biotelemetry 3:15 (neck collar, 50 Hz):
// mean VeDBA above 0.0413 g separated feeding from standing/lying (feeding
// sensitivity 98.8 %, precision 93.1 %). Gonzalez et al. 2015 (Computers and
// Electronics in Agriculture 110, collar, 10 Hz, grazing cattle) found such
// thresholds differ per animal, so this value is a starting point to be
// refined with field data from our own collars.
//
// Mapping to the existing 5-second ActivityWindow feature: the per-axis
// variance about the window mean is the dynamic acceleration power, so
// sqrt(axis_variance_sum) is the RMS dynamic vector magnitude. For roughly
// Gaussian motion mean |a_dyn| ~= 0.92 * RMS, hence
//   41.3 mg VeDBA  ->  RMS ~= 45 mg  ->  axis_variance_sum ~= 2000 mg^2.
// Differences (10 Hz vs 50 Hz, 5 s window mean vs a longer running mean)
// make this an approximation, not an equivalence.
namespace activity_classifier {
constexpr uint32_t kActiveAxisVarianceSumMg2 = 2000;
}  // namespace activity_classifier

inline bool activityWindowActive(uint32_t axis_variance_sum_mg2) {
  return axis_variance_sum_mg2 >= activity_classifier::kActiveAxisVarianceSumMg2;
}

// Head position (owner decision 2026-10-12: separate grazing from other
// movement with published rules, before the PERIODIC format goes live).
// Also RESEARCH-DERIVED and NOT validated on our collars or animals.
//
// Sources:
// - Gonzalez et al. 2015 (collar, 10 Hz, grazing steers): the mean of the
//   accelerometer axis along the neck separates head-down from head-up, the
//   movement level separates resting from activity; foraging is head down
//   with activity. Their thresholds were fitted per animal, so no number is
//   taken from them, only the rule.
// - Swiss LoRa cattle collar, arXiv 2406.06245 (2024): head angle below
//   -20 degrees = grazing, above -10 degrees = not grazing. Between the two
//   the previous state is kept here (hysteresis).
// - Diosdado et al. 2015: the movement threshold above (feeding).
// Lying is not separated: Diosdado's lying rule reads the lateral axis of a
// sensor on the side of the neck, and the ORUN box sits on top of the neck
// (solar panel up), so that rule does not transfer.
//
// The angle is the tilt of the axis that runs along the neck toward the head,
// from the window's mean acceleration (gravity): an axis pointing up reads
// +1 g, so tilt = asin(a_forward / |a|), negative with the head down.
// Rotation of the collar around the neck does not change it.
enum class NeckAxis : uint8_t {
  kUnset = 0,  // orientation not known: no head position, no grazing
  kPlusX,
  kMinusX,
  kPlusY,
  kMinusY,
  kPlusZ,
  kMinusZ,
};

enum class HeadPose : uint8_t { kUp, kDown };

namespace activity_classifier {
// sin^2 of the thresholds, x10000: sin(20 deg)^2 = 0.1170, sin(10 deg)^2 = 0.0302.
constexpr int64_t kHeadDownSinSquaredX10000 = 1170;
constexpr int64_t kHeadUpSinSquaredX10000 = 302;
// A window mean outside 0.5..1.5 g is not a usable gravity reading.
constexpr int64_t kGravityMinMg = 500;
constexpr int64_t kGravityMaxMg = 1500;
// Which LIS3DH axis runs along the neck toward the head on the ORUN collar.
// Bench step 2026-10-12 (firmware/tests/m6/activity_axis_bench.sh, collar
// 0E8ADE7E71531AA3, RAK1904 in the Unify box, panel up): level
// g_mg=-118,-79,-943; head-side edge tilted down g_mg=-37,521,-797, so the
// axis toward the head is -Y (it read -521 mg, about -33 degrees). Valid
// while every collar is assembled the same way. USB "ACTIVITY AXIS <axis>"
// overrides it until reboot.
constexpr NeckAxis kDefaultNeckForwardAxis = NeckAxis::kMinusY;
}  // namespace activity_classifier

inline const char* neckAxisName(NeckAxis axis) {
  switch (axis) {
    case NeckAxis::kPlusX: return "+X";
    case NeckAxis::kMinusX: return "-X";
    case NeckAxis::kPlusY: return "+Y";
    case NeckAxis::kMinusY: return "-Y";
    case NeckAxis::kPlusZ: return "+Z";
    case NeckAxis::kMinusZ: return "-Z";
    case NeckAxis::kUnset: break;
  }
  return "OFF";
}

// Mean acceleration along the forward axis, milli-g.
inline int32_t neckForwardComponent(int32_t x_mg, int32_t y_mg, int32_t z_mg,
                                    NeckAxis axis) {
  switch (axis) {
    case NeckAxis::kPlusX: return x_mg;
    case NeckAxis::kMinusX: return -x_mg;
    case NeckAxis::kPlusY: return y_mg;
    case NeckAxis::kMinusY: return -y_mg;
    case NeckAxis::kPlusZ: return z_mg;
    case NeckAxis::kMinusZ: return -z_mg;
    case NeckAxis::kUnset: break;
  }
  return 0;
}

// New head position from one window's mean acceleration. Keeps `previous`
// inside the -20..-10 degree band, for an unusable gravity reading, and when
// the axis is unset (callers then report no head position at all).
inline HeadPose classifyHeadPose(int32_t x_mg, int32_t y_mg, int32_t z_mg,
                                 NeckAxis axis, HeadPose previous) {
  if (axis == NeckAxis::kUnset) return previous;
  const int64_t gx = x_mg, gy = y_mg, gz = z_mg;
  const int64_t g2 = gx * gx + gy * gy + gz * gz;
  using namespace activity_classifier;
  if (g2 < kGravityMinMg * kGravityMinMg || g2 > kGravityMaxMg * kGravityMaxMg)
    return previous;
  const int64_t forward = neckForwardComponent(x_mg, y_mg, z_mg, axis);
  if (forward >= 0) return HeadPose::kUp;
  const int64_t f2 = forward * forward * 10000;
  if (f2 > g2 * kHeadDownSinSquaredX10000) return HeadPose::kDown;
  if (f2 < g2 * kHeadUpSinSquaredX10000) return HeadPose::kUp;
  return previous;
}

}  // namespace orun_tlp
