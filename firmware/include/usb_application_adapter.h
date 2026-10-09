#pragma once

#include <stdint.h>

#include "application_request.h"
#include "config_mutation.h"

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

}  // namespace orun_tlp
