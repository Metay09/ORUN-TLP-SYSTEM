#pragma once

#include <stdint.h>
#include "runtime_config.h"

namespace orun_tlp {

// Reporting follows requested tracking, not GNSS availability.
inline bool productReportingEnabled(
    bool tracker_storage, const RequestedConfig& requested) {
  return tracker_storage && requested.tracking_enabled &&
         requested.location_source == RequestedLocationSource::kGnss;
}


// Report timing is independent of GNSS acquisition success.
// An overdue report never invents a new future interval.
inline uint32_t productReportRemainingMs(
    bool open, uint32_t started, uint32_t now,
    uint32_t interval, bool fix_pending = false) {
  if (!open || !interval || fix_pending) return 0U;
  const uint32_t elapsed = now - started;
  return elapsed >= interval ? 0U : interval - elapsed;
}

// Backlog must leave room for either source of a new report.
inline uint32_t productBacklogWindowMs(
    uint32_t periodic_remaining,
    uint32_t gnss_remaining) {
  return periodic_remaining < gnss_remaining
      ? periodic_remaining : gnss_remaining;
}

inline bool productReportDue(
    bool open, uint32_t started, uint32_t now,
    uint32_t interval, bool fix_pending) {
  if (!open || !interval) return false;
  const uint32_t elapsed = now - started;

  // PERIODIC schema requires a real nonzero duration.
  if (elapsed < 1000U) return false;
  return fix_pending || elapsed >= interval;
}

}  // namespace orun_tlp
