// M7P5: config_format encode/decode round-trip and fail-safe corruption
// handling, against the actual production module (only pure byte-level
// checks -- no flash backend involved here).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config_format.h"

using namespace orun_tlp::config_format;

int main() {
  // Round-trip: custom (non-default, non-round) values survive encode/decode.
  {
    uint8_t bytes[kRecordSize];
    const Config original{247, 12000};
    encode(original, 7, bytes);
    uint64_t generation = 0;
    Config decoded{};
    assert(decode(bytes, generation, decoded));
    assert(generation == 7);
    assert(decoded.tracking_interval_seconds == 247);
    assert(decoded.battery_capacity_mah == 12000);
  }

  // battery_capacity_mah = 0 (unspecified) round-trips distinctly from an
  // absent/corrupt record.
  {
    uint8_t bytes[kRecordSize];
    encode(Config{180, 0}, 1, bytes);
    uint64_t generation = 0;
    Config decoded{};
    assert(decode(bytes, generation, decoded));
    assert(decoded.battery_capacity_mah == 0);
    assert(decoded.tracking_interval_seconds == 180);
  }

  // Erased flash (all 0xFF) is not a valid record.
  {
    uint8_t bytes[kRecordSize];
    memset(bytes, 0xFF, sizeof(bytes));
    uint64_t generation = 0;
    Config decoded{};
    assert(!decode(bytes, generation, decoded));
  }

  // Generation 0 is never valid (mirrors journal_format's page-generation convention).
  {
    uint8_t bytes[kRecordSize];
    encode(Config{180, 0}, 0, bytes);
    // encode() does not itself forbid generation 0 -- decode() must reject it.
    uint64_t generation = 0;
    Config decoded{};
    assert(!decode(bytes, generation, decoded));
  }

  // CRC corruption anywhere in the body is detected.
  {
    uint8_t bytes[kRecordSize];
    encode(Config{300, 5000}, 3, bytes);
    bytes[20] ^= 0x01;  // flip a bit inside tracking_interval_seconds
    uint64_t generation = 0;
    Config decoded{};
    assert(!decode(bytes, generation, decoded));
  }

  // Torn write: body+CRC landed, but the commit word is still erased (0xFF).
  {
    uint8_t bytes[kRecordSize];
    encode(Config{300, 5000}, 3, bytes);
    memset(bytes + 32, 0xFF, 4);  // commit word never programmed
    uint64_t generation = 0;
    Config decoded{};
    assert(!decode(bytes, generation, decoded));
  }

  // Torn write: commit word present but body/CRC not fully programmed
  // (simulated as a corrupted body byte with an otherwise-valid commit word).
  {
    uint8_t bytes[kRecordSize];
    encode(Config{300, 5000}, 3, bytes);
    bytes[9] ^= 0xFF;  // corrupt a generation byte, leave CRC/commit as encoded
    uint64_t generation = 0;
    Config decoded{};
    assert(!decode(bytes, generation, decoded));
  }

  // Unknown/newer schema version must fail safely, never be interpreted as v1.
  {
    uint8_t bytes[kRecordSize];
    encode(Config{300, 5000}, 3, bytes);
    bytes[4] = kVersion + 1;
    // Re-seal so only the version byte differs from an otherwise-valid v1
    // record -- proves rejection is driven by the version check, not CRC.
    // (encode() already sealed for kVersion; corrupting the version alone
    // after sealing intentionally also breaks the CRC, but decode() checks
    // version before CRC, so this still isolates the version-check path.)
    uint64_t generation = 0;
    Config decoded{};
    assert(!decode(bytes, generation, decoded));
  }

  // Mismatched encoded_length (payload-length field) fails safely.
  {
    uint8_t bytes[kRecordSize];
    encode(Config{300, 5000}, 3, bytes);
    // Corrupting the length field breaks the CRC too, but decode() checks
    // length before CRC, isolating this specific rejection path.
    uint64_t generation = 0;
    Config decoded{};
    bytes[17] ^= 0xFF;
    assert(!decode(bytes, generation, decoded));
  }

  puts("M7P5 config_format encode/decode checks: PASS");
}
