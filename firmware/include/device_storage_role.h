#pragma once

#include <stdint.h>

#include "flash_backend.h"
#include "product_region.h"

// Y2a (owner decision 2026-11-03): ONE firmware for Tracker and Gateway. The
// two jobs use the same flash regions in different formats:
//   - security region (2 pages): Tracker SecurityStore "ORS1" pages, or the
//     Gateway's signed grant snapshot "OGK1" pages;
//   - product region (32 pages): Tracker ObservationStore pages, or the
//     Gateway's custody store pages.
// The storage role is decided once per boot, BEFORE either store is opened:
//   1. What is already written decides. Each store also refuses the other
//      store's pages on its own (product_region::mountDecision,
//      GatewayGrantSnapshotReader), so a wrong guess can never reformat data;
//      this decision only matters for regions that are still blank.
//   2. Blank regions: the saved service intent decides (APP SERVICES ... A =
//      Gateway; T, R, TR or NONE = Tracker). The intent is chosen when the
//      device is set up ("Cihaz ekle"), suggested by whether GNSS is fitted.
//   3. No intent (AUTO) on blank regions: UNASSIGNED. Neither store is opened
//      and nothing is written until the role is chosen; then the device
//      restarts into that role.
//   4. Tracker evidence and Gateway evidence together: CONFLICT. Nothing is
//      opened or written; an operator must look.
namespace orun_tlp {

enum class SecurityRegionContent : uint8_t {
  kErased,   // both page heads erased
  kTracker,  // an ORS1 page (any version)
  kGateway,  // an OGK1 page (complete or torn)
  kMixed,    // ORS1 and OGK1 pages together
  kUnknown,  // anything else, no ORS1/OGK1 page
};

// From the first 4 bytes (big-endian magic) of each of the two pages.
SecurityRegionContent classifySecurityPageHeads(const uint8_t page0[4],
                                                const uint8_t page1[4]);
// Reads the two page heads (offsets 0 and 4096). false on a read failure.
bool readSecurityRegionContent(const FlashBackend& security_port,
                               SecurityRegionContent& out);
const char* securityRegionContentName(SecurityRegionContent content);

enum class StorageRole : uint8_t { kUnassigned, kTracker, kGateway, kConflict };

struct StorageRoleInput {
  bool security_readable;
  SecurityRegionContent security;
  bool product_readable;
  product_region::Census product;
  uint8_t service_mode;        // config_format::kServiceMode*
  uint8_t requested_services;  // config_format::kService* bits
};

StorageRole decideStorageRole(const StorageRoleInput& input);
// The role the saved intent alone asks for on blank regions (step 2/3).
StorageRole storageRoleForIntent(uint8_t service_mode,
                                 uint8_t requested_services);
const char* storageRoleName(StorageRole role);

}  // namespace orun_tlp
