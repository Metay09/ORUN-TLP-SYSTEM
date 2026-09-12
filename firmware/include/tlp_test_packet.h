#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp::tlp {

constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kPacketTypeTest = 0x01;
constexpr size_t kTestPacketSize = 18;

struct TestPacket {
  uint64_t source_device_id;
  uint32_t sequence_number;
  uint32_t uptime_ms;
};

bool serializeTestPacket(const TestPacket& packet, uint8_t* output,
                         size_t output_size);
bool deserializeTestPacket(const uint8_t* input, size_t input_size,
                           TestPacket* packet);

}  // namespace orun_tlp::tlp
