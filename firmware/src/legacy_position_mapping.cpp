#include "legacy_position_mapping.h"

#include "tlp_position_packet.h"

namespace orun_tlp {

bool encodeLegacyPosition(const GnssFix& fix, DeviceIdentity source,
                          uint32_t sequence, uint8_t* output,
                          size_t output_size) {
  const tlp::PositionPacket packet{
      source.legacyUint64(), sequence, fix.utc_epoch_seconds, fix.latitude_e7,
      fix.longitude_e7, fix.altitude_mm, fix.hdop_x100, fix.satellites,
      fix.flags};
  return tlp::serializePositionPacket(packet, output, output_size);
}

}  // namespace orun_tlp
