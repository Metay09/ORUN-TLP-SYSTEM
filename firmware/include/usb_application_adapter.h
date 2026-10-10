#pragma once

#include <stdint.h>

#include "application_request.h"
#include "config_mutation.h"
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

//   APP SERVICES?   requested intent next to what actually runs
bool isUsbServicesQuery(const char* text, uint8_t length);

struct UsbServiceStatus {
  uint8_t service_mode = 0;
  uint8_t requested_services = 0;
  // false when an EXPLICIT intent saved by other firmware cannot run here
  // and the legacy GNSS-based role is in effect instead.
  bool intent_applied = false;
  ServiceStatus tracking;
  ServiceStatus relay_forwarding;
  bool application_receive = false;
  NodeRole legacy_role = NodeRole::kBase;
};

void printUsbServiceStatus(const UsbServiceStatus& status);

}  // namespace orun_tlp
