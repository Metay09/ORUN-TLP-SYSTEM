#include "device_storage_role.h"

#include "config_format.h"

namespace orun_tlp {
namespace {

constexpr uint32_t kOrs1 = 0x4F525331U;  // security_format::kMagic
constexpr uint32_t kOgk1 = 0x4F474B31U;  // gateway_grant_format::kMagic
constexpr uint32_t kPageBytes = 4096U;

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24U) |
         (static_cast<uint32_t>(p[1]) << 16U) |
         (static_cast<uint32_t>(p[2]) << 8U) | static_cast<uint32_t>(p[3]);
}

}  // namespace

SecurityRegionContent classifySecurityPageHeads(const uint8_t page0[4],
                                                const uint8_t page1[4]) {
  bool tracker = false, gateway = false, other = false;
  const uint8_t* heads[2] = {page0, page1};
  for (const uint8_t* h : heads) {
    const uint32_t magic = be32(h);
    if (magic == UINT32_MAX) continue;
    if (magic == kOrs1) tracker = true;
    else if (magic == kOgk1) gateway = true;
    else other = true;
  }
  if (tracker && gateway) return SecurityRegionContent::kMixed;
  if (tracker) return SecurityRegionContent::kTracker;
  if (gateway) return SecurityRegionContent::kGateway;
  return other ? SecurityRegionContent::kUnknown
               : SecurityRegionContent::kErased;
}

bool readSecurityRegionContent(const FlashBackend& security_port,
                               SecurityRegionContent& out) {
  uint8_t page0[4], page1[4];
  if (!security_port.read(0U, page0, sizeof(page0)) ||
      !security_port.read(kPageBytes, page1, sizeof(page1)))
    return false;
  out = classifySecurityPageHeads(page0, page1);
  return true;
}

const char* securityRegionContentName(SecurityRegionContent content) {
  switch (content) {
    case SecurityRegionContent::kErased: return "ERASED";
    case SecurityRegionContent::kTracker: return "ORS1";
    case SecurityRegionContent::kGateway: return "OGK1";
    case SecurityRegionContent::kMixed: return "MIXED";
    case SecurityRegionContent::kUnknown: return "UNKNOWN";
  }
  return "?";
}

StorageRole storageRoleForIntent(uint8_t service_mode,
                                 uint8_t requested_services) {
  namespace cf = config_format;
  if (service_mode != cf::kServiceModeExplicit) return StorageRole::kUnassigned;
  if ((requested_services & static_cast<uint8_t>(~cf::kKnownServicesMask)) != 0)
    return StorageRole::kUnassigned;
  const bool receive = (requested_services & cf::kServiceApplicationReceive) != 0;
  const bool other = (requested_services &
                      (cf::kServiceTracking | cf::kServiceRelayForwarding)) != 0;
  if (receive && other) return StorageRole::kUnassigned;  // not a valid intent
  return receive ? StorageRole::kGateway : StorageRole::kTracker;
}

StorageRole decideStorageRole(const StorageRoleInput& in) {
  const bool sec_tracker = in.security_readable &&
      (in.security == SecurityRegionContent::kTracker ||
       in.security == SecurityRegionContent::kMixed);
  const bool sec_gateway = in.security_readable &&
      (in.security == SecurityRegionContent::kGateway ||
       in.security == SecurityRegionContent::kMixed);
  const bool product_tracker = in.product_readable && in.product.observation > 0U;
  const bool product_gateway = in.product_readable &&
      (in.product.custody > 0U || in.product.product_custody > 0U);
  const bool tracker = sec_tracker || product_tracker;
  const bool gateway = sec_gateway || product_gateway;
  if (tracker && gateway) return StorageRole::kConflict;
  if (gateway) return StorageRole::kGateway;
  if (tracker) return StorageRole::kTracker;
  // Nothing of either store is written (unknown leftovers are not evidence:
  // the chosen store refuses them until they are explicitly prepared).
  return storageRoleForIntent(in.service_mode, in.requested_services);
}

const char* storageRoleName(StorageRole role) {
  switch (role) {
    case StorageRole::kUnassigned: return "UNASSIGNED";
    case StorageRole::kTracker: return "TRACKER";
    case StorageRole::kGateway: return "GATEWAY";
    case StorageRole::kConflict: return "CONFLICT";
  }
  return "?";
}

}  // namespace orun_tlp
