#pragma once

#include <stdint.h>

#include "application_request.h"

namespace orun_tlp {

// M7P7H USB transport adapter helpers. Product operation semantics stay in
// ApplicationRequestService; this module owns only the human-readable USB
// command spelling and result formatting.
bool parseUsbApplicationQuery(const char* text, uint8_t length,
                              ApplicationRequestKind* kind);

void printUsbApplicationResponse(const ApplicationResponse& response);

}  // namespace orun_tlp
