// Y2a: one firmware, storage role decided at boot from what is written, then
// from the saved service intent. Host only.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "config_format.h"
#include "device_storage_role.h"

using namespace orun_tlp;
namespace cf = orun_tlp::config_format;

namespace {

const uint8_t kErased[4] = {0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t kOrs1[4] = {'O', 'R', 'S', '1'};
const uint8_t kOgk1[4] = {'O', 'G', 'K', '1'};
const uint8_t kJunk[4] = {0x12, 0x34, 0x56, 0x78};

product_region::Census census(uint16_t observation, uint16_t custody,
                              uint16_t product_custody, uint16_t unknown) {
  product_region::Census c{};
  c.observation = observation;
  c.custody = custody;
  c.product_custody = product_custody;
  c.unknown = unknown;
  c.erased = static_cast<uint16_t>(32U - observation - custody - product_custody - unknown);
  return c;
}

StorageRoleInput input(SecurityRegionContent sec, product_region::Census p,
                       uint8_t mode = cf::kServiceModeAuto, uint8_t services = 0U) {
  StorageRoleInput in{};
  in.security_readable = true;
  in.security = sec;
  in.product_readable = true;
  in.product = p;
  in.service_mode = mode;
  in.requested_services = services;
  return in;
}

void testClassify() {
  assert(classifySecurityPageHeads(kErased, kErased) == SecurityRegionContent::kErased);
  assert(classifySecurityPageHeads(kOrs1, kErased) == SecurityRegionContent::kTracker);
  assert(classifySecurityPageHeads(kErased, kOrs1) == SecurityRegionContent::kTracker);
  assert(classifySecurityPageHeads(kOgk1, kOgk1) == SecurityRegionContent::kGateway);
  assert(classifySecurityPageHeads(kOrs1, kOgk1) == SecurityRegionContent::kMixed);
  assert(classifySecurityPageHeads(kJunk, kErased) == SecurityRegionContent::kUnknown);
  assert(classifySecurityPageHeads(kOgk1, kJunk) == SecurityRegionContent::kGateway);
}

struct TwoPages : FlashBackend {
  std::array<uint8_t, 8192U> bytes{};
  TwoPages() { bytes.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t o, void* out, size_t n) const override {
    if (!out || o > bytes.size() || n > bytes.size() - o) return false;
    memcpy(out, bytes.data() + o, n);
    return true;
  }
  FlashOpResult program(uint32_t, const void*, size_t) override { return FlashOpResult::kFailed; }
  FlashOpResult erasePage(uint32_t) override { return FlashOpResult::kFailed; }
};

void testReadRegion() {
  TwoPages flash;
  SecurityRegionContent c = SecurityRegionContent::kUnknown;
  assert(readSecurityRegionContent(flash, c) && c == SecurityRegionContent::kErased);
  memcpy(flash.bytes.data() + 4096U, kOgk1, 4U);
  assert(readSecurityRegionContent(flash, c) && c == SecurityRegionContent::kGateway);
  memcpy(flash.bytes.data(), kOrs1, 4U);
  assert(readSecurityRegionContent(flash, c) && c == SecurityRegionContent::kMixed);
}

void testWrittenDataDecides() {
  // Today's devices: what they already hold decides, whatever is saved.
  assert(decideStorageRole(input(SecurityRegionContent::kTracker, census(3, 0, 0, 0))) ==
         StorageRole::kTracker);
  assert(decideStorageRole(input(SecurityRegionContent::kTracker, census(0, 0, 0, 0),
                                 cf::kServiceModeExplicit, cf::kServiceApplicationReceive)) ==
         StorageRole::kTracker);
  assert(decideStorageRole(input(SecurityRegionContent::kErased, census(5, 0, 0, 0))) ==
         StorageRole::kTracker);   // records, credential not given yet
  assert(decideStorageRole(input(SecurityRegionContent::kGateway, census(0, 0, 2, 0))) ==
         StorageRole::kGateway);
  assert(decideStorageRole(input(SecurityRegionContent::kErased, census(0, 0, 2, 0),
                                 cf::kServiceModeExplicit, cf::kServiceTracking)) ==
         StorageRole::kGateway);   // held records, signed grant not yet installed
  assert(decideStorageRole(input(SecurityRegionContent::kErased, census(0, 1, 0, 0))) ==
         StorageRole::kGateway);   // legacy custody pages
  assert(decideStorageRole(input(SecurityRegionContent::kGateway, census(0, 0, 0, 0))) ==
         StorageRole::kGateway);
  // Both kinds of evidence: never guess.
  assert(decideStorageRole(input(SecurityRegionContent::kTracker, census(0, 0, 2, 0))) ==
         StorageRole::kConflict);
  assert(decideStorageRole(input(SecurityRegionContent::kMixed, census(0, 0, 0, 0))) ==
         StorageRole::kConflict);
  assert(decideStorageRole(input(SecurityRegionContent::kGateway, census(4, 0, 0, 0))) ==
         StorageRole::kConflict);
}

void testBlankDeviceAsksTheIntent() {
  const auto blank = census(0, 0, 0, 0);
  assert(decideStorageRole(input(SecurityRegionContent::kErased, blank)) ==
         StorageRole::kUnassigned);                                   // AUTO
  assert(decideStorageRole(input(SecurityRegionContent::kErased, blank,
                                 cf::kServiceModeExplicit, cf::kServiceApplicationReceive)) ==
         StorageRole::kGateway);
  assert(decideStorageRole(input(SecurityRegionContent::kErased, blank,
                                 cf::kServiceModeExplicit, cf::kServiceTracking)) ==
         StorageRole::kTracker);
  assert(decideStorageRole(input(SecurityRegionContent::kErased, blank,
                                 cf::kServiceModeExplicit,
                                 cf::kServiceTracking | cf::kServiceRelayForwarding)) ==
         StorageRole::kTracker);
  assert(decideStorageRole(input(SecurityRegionContent::kErased, blank,
                                 cf::kServiceModeExplicit, 0U)) == StorageRole::kTracker);
  assert(decideStorageRole(input(SecurityRegionContent::kErased, blank,
                                 cf::kServiceModeExplicit,
                                 cf::kServiceApplicationReceive | cf::kServiceTracking)) ==
         StorageRole::kUnassigned);                                   // invalid intent
  // Unknown leftovers are not evidence: the intent still decides.
  assert(decideStorageRole(input(SecurityRegionContent::kUnknown, census(0, 0, 0, 32))) ==
         StorageRole::kUnassigned);
  // Unreadable regions are not evidence either.
  StorageRoleInput unread = input(SecurityRegionContent::kGateway, census(0, 0, 3, 0));
  unread.security_readable = false;
  unread.product_readable = false;
  assert(decideStorageRole(unread) == StorageRole::kUnassigned);
  assert(strcmp(storageRoleName(StorageRole::kGateway), "GATEWAY") == 0);
}

}  // namespace

int main() {
  testClassify();
  testReadRegion();
  testWrittenDataDecides();
  testBlankDeviceAsksTheIntent();
  puts("Y2a device storage role: written data decides, blank device follows "
       "the saved intent, conflicts never guessed: PASS (HOST ONLY)");
  return 0;
}
