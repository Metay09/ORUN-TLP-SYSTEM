// Host-only exact Step 3A real USB parser regression. Synthetic bytes below
// are not an enrolled key and are NEVER linked into the target firmware.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "crc32_ieee.h"
#include "tracker_real_enrollment.h"

using orun_tlp::TrackerRealProvisionParse;

std::string hex(const uint8_t* bytes, size_t count) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(count * 2);
  for (size_t i = 0; i < count; ++i) {
    out.push_back(kHex[bytes[i] >> 4U]);
    out.push_back(kHex[bytes[i] & 15U]);
  }
  return out;
}

int main() {
  uint8_t raw[60]{};
  for (unsigned i = 0; i < 16; ++i) raw[i] = static_cast<uint8_t>(0x20 + i);
  raw[19] = 1U;  // epoch=1 big endian
  for (unsigned i = 0; i < 32; ++i) raw[20+i] = static_cast<uint8_t>(0x60 + i);
  const uint8_t gw[8] = {0x09, 0xA4, 0x62, 0xBD, 0x4B, 0x27, 0x5B, 0xA5};
  memcpy(raw + 52, gw, sizeof(gw));
  const uint32_t crc = orun_tlp::crc32Ieee(raw, sizeof(raw));
  char crc_text[9];
  snprintf(crc_text, sizeof(crc_text), "%08X", crc);
  std::string line = "BENCH PROVISION " + hex(raw,16) +
      " 00000001 " + hex(raw + 20,32) + " " + hex(gw,8) +
      " " + crc_text;

  uint8_t id[16]{}, root[32]{};
  uint32_t epoch = 0;
  uint64_t gateway = 0;
  auto parse = [&](const std::string& input) {
    return orun_tlp::parseTrackerRealProvision(
        input.data(), input.size(), id, epoch, root, gateway);
  };
  assert(parse(line) == TrackerRealProvisionParse::kOk);
  assert(memcmp(id, raw, 16) == 0);
  assert(memcmp(root, raw + 20, 32) == 0);
  assert(epoch == 1U);
  assert(gateway == UINT64_C(0x09A462BD4B275BA5));

  std::string bad_crc = line;
  bad_crc[bad_crc.size() - 1] =
      bad_crc.back() == '0' ? '1' : '0';
  assert(parse(bad_crc) == TrackerRealProvisionParse::kBadCrc);

  std::string truncated = line.substr(0, line.size()-1);
  assert(parse(truncated) == TrackerRealProvisionParse::kMalformed);
  std::string bad_hex = line;
  bad_hex[17] = 'X';
  assert(parse(bad_hex) == TrackerRealProvisionParse::kMalformed);
  assert(orun_tlp::parseTrackerRealProvision(
      nullptr, line.size(), id, epoch, root, gateway) ==
      TrackerRealProvisionParse::kMalformed);
  printf("ORUN_PHASE2_REAL_USB_PARSER_PASS\n");
}
