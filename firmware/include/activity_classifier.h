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

}  // namespace orun_tlp
