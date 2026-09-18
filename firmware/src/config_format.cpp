#include "config_format.h"
#include <string.h>
#include "journal_format.h"

namespace orun_tlp::config_format {
namespace jf = orun_tlp::journal_format;
namespace {
// Body is bytes [0, kCrcOffset); CRC lands at kCrcOffset, commit word at
// kCommitOffset -- programmed as two separate flash writes by ConfigStore
// (body+CRC first, commit word last), exactly mirroring journal_format's
// seal()/sealed() pattern (encodeState/decodeState).
constexpr unsigned kCrcOffset = 28;
constexpr unsigned kCommitOffset = 32;

void seal(uint8_t* bytes) {
  jf::put32(bytes + kCrcOffset, jf::crc32(bytes, kCrcOffset));
  jf::put32(bytes + kCommitOffset, kCommit);
}
bool sealed(const uint8_t* bytes) {
  return jf::get32(bytes + kCommitOffset) == kCommit &&
         jf::get32(bytes + kCrcOffset) == jf::crc32(bytes, kCrcOffset);
}
}  // namespace

void encode(const Config& config, uint64_t generation, uint8_t* bytes) {
  memset(bytes, 0, kRecordSize);
  jf::put32(bytes, kMagic);
  bytes[4] = kVersion;
  jf::put64(bytes + 8, generation);
  jf::put16(bytes + 16, kPayloadSize);
  jf::put32(bytes + 20, config.tracking_interval_seconds);
  jf::put32(bytes + 24, config.battery_capacity_mah);
  seal(bytes);
}

bool decode(const uint8_t* bytes, uint64_t& generation, Config& config) {
  if (jf::get32(bytes) != kMagic) return false;
  // bytes[4] is the version; bytes[5..7] are reserved and must be zero. An
  // unrecognized/newer version fails closed here -- it is never interpreted
  // as v1, satisfying the "unknown/newer schema must fail safely" contract.
  if (bytes[4] != kVersion || bytes[5] || bytes[6] || bytes[7]) return false;
  const uint64_t candidate_generation = jf::get64(bytes + 8);
  if (candidate_generation == 0) return false;
  if (jf::get16(bytes + 16) != kPayloadSize || jf::get16(bytes + 18) != 0) return false;
  if (!sealed(bytes)) return false;
  generation = candidate_generation;
  config.tracking_interval_seconds = jf::get32(bytes + 20);
  config.battery_capacity_mah = jf::get32(bytes + 24);
  return true;
}

}  // namespace orun_tlp::config_format
