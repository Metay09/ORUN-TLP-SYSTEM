// KQ05 OCQ2/v4 supplemental Gateway identity binding: native PC only.
// No device Flash, no provisioning, no Gateway ACK/cryptographic authority.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "custody_store_product_format.h"
namespace fmt = orun_tlp::custody_store_product_format;
namespace {

constexpr uint64_t kGatewayId = UINT64_C(0x4757415949443031);

void testLegacyV3CompatiblePrimary() {
  uint8_t header[fmt::kPageHeaderSize]{};
  assert(fmt::encodePageHeader(10U, header));
  fmt::PageInspection got{};
  assert(fmt::inspectPageHeader(header, sizeof(header), got));
  assert(got.evidence == fmt::PageEvidence::kPrepared);
  assert(!got.gateway_bound && got.gateway_id == 0U);
  memset(header + fmt::kHeaderActiveOffset, 0U, 4U);
  assert(fmt::inspectPageHeader(header, sizeof(header), got));
  assert(got.evidence == fmt::PageEvidence::kActive);
  assert(got.generation == 10U && !got.gateway_bound);
}

void testBoundHeaderRoundTripAndCrashStates() {
  uint8_t header[fmt::kPageHeaderSize]{};
  memset(header, 0xA5U, sizeof(header));
  assert(!fmt::encodeBoundPageHeader(0U, kGatewayId, header));
  for (uint8_t byte : header) assert(byte == 0xA5U);
  assert(!fmt::encodeBoundPageHeader(5U, 0U, header));
  assert(!fmt::encodeBoundPageHeader(5U, UINT64_MAX, header));
  assert(!fmt::encodeBoundPageHeader(UINT64_MAX, kGatewayId, header));
  for (uint8_t byte : header) assert(byte == 0xA5U);

  assert(fmt::encodeBoundPageHeader(23U, kGatewayId, header));
  fmt::PageInspection parsed{};
  assert(fmt::inspectPageHeader(header, sizeof(header), parsed));
  assert(parsed.evidence == fmt::PageEvidence::kPrepared);
  assert(parsed.gateway_bound && parsed.gateway_id == kGatewayId);
  assert(parsed.generation == 23U);
  // Simulate writing 0..31, then 40..63, but losing power before
  // committing the header. It is NOT an activated custody page.
  memset(header + fmt::kHeaderCommitOffset, 0xFF, 4U);
  assert(fmt::inspectPageHeader(header, sizeof(header), parsed));
  assert(parsed.evidence == fmt::PageEvidence::kStaged);
  memset(header + fmt::kHeaderCommitOffset, 0U, 4U);
  memset(header + fmt::kHeaderActiveOffset, 0U, 4U);
  assert(fmt::inspectPageHeader(header, sizeof(header), parsed));
  assert(parsed.evidence == fmt::PageEvidence::kActive);
  assert(parsed.gateway_bound && parsed.gateway_id == kGatewayId);

  // Any flip in ID, its complement, CRC or marker invalidates the page.
  const unsigned locations[] = {
    fmt::kGatewayIdOffset,
    fmt::kGatewayIdComplementOffset,
    fmt::kBindingCrcOffset,
    fmt::kBindingMarkerOffset,
  };
  for (unsigned off : locations) {
    const uint8_t saved = header[off];
    header[off] ^= 0x01U;
    assert(fmt::inspectPageHeader(header, sizeof(header), parsed));
    assert(parsed.evidence == fmt::PageEvidence::kCorrupt);
    header[off] = saved;
  }
  assert(fmt::inspectPageHeader(header, sizeof(header), parsed));
  assert(parsed.evidence == fmt::PageEvidence::kActive);
}

void testHeaderRejectsWrongVersionAndTail() {
  uint8_t old[fmt::kPageHeaderSize]{};
  assert(fmt::encodePageHeader(11U, old));
  memset(old + fmt::kHeaderActiveOffset, 0U, 4U);
  // v3 must keep all 24 reserved bytes erased: can't be misread as v4.
  old[fmt::kGatewayIdOffset] = 0U;
  fmt::PageInspection inspection{};
  assert(fmt::inspectPageHeader(old, sizeof(old), inspection));
  assert(inspection.evidence == fmt::PageEvidence::kCorrupt);
  old[fmt::kGatewayIdOffset] = 0xFFU;
  assert(fmt::inspectPageHeader(old, sizeof(old), inspection));
  assert(inspection.evidence == fmt::PageEvidence::kActive &&
         !inspection.gateway_bound);
}

}  // namespace

int main() {
  testLegacyV3CompatiblePrimary();
  testBoundHeaderRoundTripAndCrashStates();
  testHeaderRejectsWrongVersionAndTail();
  puts("KQ05 OCQ2/v4 Gateway-bound header, CRC, v3 compatibility and torn commit: PASS (HOST ONLY)");
  puts("NO DEVICE FLASH, NO SECURITY KEY, NO GATEWAY ACK");
  return 0;
}
