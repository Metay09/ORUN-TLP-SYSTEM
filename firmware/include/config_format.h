#pragma once
#include <stddef.h>
#include <stdint.h>

// M7P5: on-flash durable config record format. A sibling of
// journal_format.h (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md sec 12
// requires the config partition to reuse history's proven power-cut
// invariant family, not its storage ownership/code/pages): explicit magic,
// schema version, generation, encoded length, payload, CRC, and a final
// 4-byte commit word programmed separately last. Reuses journal_format's
// already-tested byte-order/CRC32/erased-check primitives directly instead
// of duplicating them.
namespace orun_tlp::config_format {

constexpr uint32_t kMagic = 0x4F524331;  // "ORC1"
constexpr uint8_t kVersion = 1;
constexpr uint32_t kCommit = 0;
// v1 payload: tracking_interval_seconds (4B) + battery_capacity_mah (4B).
constexpr uint16_t kPayloadSize = 8;
// magic(4) + version+reserved(4) + generation(8) + length+reserved(4) +
// payload(8) + crc32(4) + commit(4).
constexpr uint32_t kRecordSize = 36;

struct Config {
  // Explicit constructor (not default member initializers): the vendored
  // RAK toolchain builds this firmware under gnu++11, where a struct with
  // default member initializers is not an aggregate and brace-init like
  // Config{a, b} would not compile -- matches the same pattern already used
  // by RequestedConfig/CapabilityState in runtime_config.h.
  constexpr Config(uint32_t tracking_interval_seconds_value = 0,
                   uint32_t battery_capacity_mah_value = 0)
      : tracking_interval_seconds(tracking_interval_seconds_value),
        battery_capacity_mah(battery_capacity_mah_value) {}

  uint32_t tracking_interval_seconds;
  uint32_t battery_capacity_mah;
};

// bytes must point at kRecordSize writable/readable bytes.
void encode(const Config& config, uint64_t generation, uint8_t* bytes);

// Fails closed (returns false, leaves generation/config untouched) on a bad
// magic, an unrecognized/newer schema version, a payload-length mismatch, a
// zero generation, nonzero reserved bytes, or a CRC/commit mismatch (torn
// or never-written record). Never guesses at an unknown schema's layout.
bool decode(const uint8_t* bytes, uint64_t& generation, Config& config);

}  // namespace orun_tlp::config_format
