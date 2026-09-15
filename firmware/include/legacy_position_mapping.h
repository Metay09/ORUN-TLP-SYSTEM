#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_identity.h"
#include "gnss_fix.h"

namespace orun_tlp {

// Pure compatibility mapping from the portable GNSS value to the frozen v1
// POSITION wire representation. No transport, driver, persistence or timing
// behavior belongs here.
bool encodeLegacyPosition(const GnssFix& fix, DeviceIdentity source,
                          uint32_t sequence, uint8_t* output,
                          size_t output_size);

}  // namespace orun_tlp
