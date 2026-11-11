// 6: shape of a LoRa command frame (lora_command.h); no keys, no state, so
// the Gateway's relay can use it without the Tracker's command service.
#include "lora_command.h"

namespace orun_tlp {

namespace {
uint16_t getBe16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
}  // namespace

namespace lora_command {
bool wellFormed(const uint8_t* frame, size_t size) {
  if (!isCommandFrame(frame, size) || size < kMinFrameSize || size > kMaxFrameSize ||
      frame[2] != 0U)
    return false;
  if (frame[3] == kResourceSettings) {
    const size_t n = frame[32];
    return n >= 1U && n <= kMaxEntries && size == kHeaderSize + n * kEntrySize + kTagSize;
  }
  if (frame[3] != kResourceAreas) return false;
  const uint8_t part = frame[32], parts = frame[33], areas = frame[34];
  const uint16_t vertices = getBe16(frame + 35);
  if (areas > geofence_format::kMaximumAreas ||
      vertices > geofence_format::kMaximumTotalVertices || ((areas == 0U) != (vertices == 0U)))
    return false;
  const size_t total = areaSetSize(areas, vertices);
  const size_t want_parts = (total + kAreaChunk - 1U) / kAreaChunk;
  if (parts != want_parts || part >= parts) return false;
  const size_t chunk = part + 1U < parts ? kAreaChunk : total - size_t(part) * kAreaChunk;
  return size == kAreaHeaderSize + chunk + kTagSize;
}
}  // namespace lora_command

}  // namespace orun_tlp
