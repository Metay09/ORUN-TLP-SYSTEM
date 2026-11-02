#pragma once

namespace orun_tlp {

// Y1: device builds carry the git version they were built from (scripts/
// build_id.py), so the app and USB show which firmware is running. Host test
// builds keep the fixed string. Must fit the 16-byte status field.
#ifdef ORUN_BUILD_ID
constexpr char kFirmwareVersion[] = "0.5-" ORUN_BUILD_ID;
#else
constexpr char kFirmwareVersion[] = "0.5.0-alpha";
#endif

}  // namespace orun_tlp
