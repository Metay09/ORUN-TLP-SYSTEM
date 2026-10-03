#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tlp_position_packet.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  orun_tlp::tlp::PositionPacket decoded{};
  if (!orun_tlp::tlp::deserializePositionPacket(data, size, &decoded)) return 0;

  uint8_t encoded[orun_tlp::tlp::kPositionPacketSize]{};
  if (!orun_tlp::tlp::serializePositionPacket(
          decoded, encoded, sizeof(encoded))) {
    __builtin_trap();
  }

  if (memcmp(encoded, data, sizeof(encoded)) != 0) __builtin_trap();
  return 0;
}
