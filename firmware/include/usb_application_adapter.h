#pragma once

#include <stdint.h>

#include "application_request.h"
#include "config_mutation.h"
#include "geofence_mutation.h"
#include "geofence_staging.h"
#include "node_role.h"
#include "runtime_config.h"

namespace orun_tlp {

// M7P7H USB transport adapter helpers. Product operation semantics stay in
// ApplicationRequestService; this module owns only the human-readable USB
// command spelling and result formatting.
bool parseUsbApplicationQuery(const char* text, uint8_t length,
                              ApplicationRequestKind* kind);

void printUsbApplicationResponse(const ApplicationResponse& response);

// Local USB spelling of a configuration change. The semantics, admission and
// result live in ConfigMutationOwner; this module owns only the text.
//
//   APP INTERVAL <seconds>     decimal, no sign, no padding spaces
enum class UsbConfigCommandParse : uint8_t {
  kNotConfigCommand,  // some other command; leave it to the next parser
  kMalformed,         // recognized command with an unusable argument
  kOk,
};

UsbConfigCommandParse parseUsbConfigCommand(const char* text, uint8_t length,
                                            ConfigMutationKind* kind,
                                            uint32_t* value);

void printUsbConfigMutationResult(const ConfigMutationResult& result);

//   APP SERVICES AUTO|NONE|<letters>   T tracking, R relay, A receive
// Letters each at most once, any order (service_intent.h). Admission of the
// combination is ConfigMutationOwner's decision, not the parser's.
UsbConfigCommandParse parseUsbServicesCommand(const char* text, uint8_t length,
                                              uint8_t* service_mode,
                                              uint8_t* requested_services);

// "APP SERVICES?" and "APP POWER?" are typed read-only queries
// (parseUsbApplicationQuery); BLE carries the same snapshots.

// Local USB spelling of a geofence change (GeofenceMutationOwner owns the
// semantics). A replacement is staged vertex by vertex, then committed whole:
//   APP FENCE BEGIN            new candidate, first area open
//   APP FENCE V <lat> <lon>    one vertex, decimal degrees, <= 7 decimals
//   APP FENCE AREA             start the next area (permitted union)
//   APP FENCE COMMIT           REPLACE_SET with the staged set
//   APP FENCE ABORT            drop the candidate
//   APP FENCE CLEAR            CLEAR_SET (no geofence)
enum class UsbFenceCommand : uint8_t {
  kNotFenceCommand,
  kMalformed,
  kBegin,
  kVertex,
  kArea,
  kCommit,
  kAbort,
  kClear,
};

UsbFenceCommand parseUsbFenceCommand(const char* text, uint8_t length,
                                     GeoPointE7* vertex);
void printUsbFenceStaging(const GeofenceStaging& staging);
void printUsbFenceRejected(GeofenceStaging::Result reason);
void printUsbGeofenceMutationResult(const GeofenceMutationResult& result);

}  // namespace orun_tlp
