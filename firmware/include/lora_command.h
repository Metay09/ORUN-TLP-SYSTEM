#pragma once

// 6 (owner decision 2026-11-11): settings and the area list over LoRa.
//
// The account server (or, offline, the owner's phone) signs a command with
// the per-Tracker write key the phones use over Bluetooth (ble_write_auth.h:
// K_w from K_root, never K_root itself). Gateways only carry the bytes; they
// hold no authority. A command is bound to the Tracker's current saved state
// token of its resource (settings: ConfigStore, areas: GeofenceStore): once
// applied the token moves, so a replayed or overtaken command changes nothing
// and is answered STALE with the current token (the sender signs again).
// All settings of one command are written in ONE save; an area list is
// replaced whole (or cleared), all or nothing. Radio settings are never
// accepted here (settings_table.h radio rows).
//
// COMMAND (gateway -> Tracker), big-endian, common header:
//   0  version 0x02          1  type 0x07
//   2  flags (0)             3  resource (1 = settings, 2 = areas)
//   4  target device id u64  12 key epoch u32      16 command id u32
//   20 expected token of the resource: incarnation u64, 28 revision u32
// resource 1 (settings), one frame:
//   32 n (1..8)              33 n x { setting id u8, value u32 }
// resource 2 (areas), 1..3 frames ("parts") carrying one area set S:
//   S = n_areas u8 || vertex count of each area (n x u8) ||
//       every vertex { latitude e7 i32, longitude e7 i32 }
//   (n_areas = 0 and no vertices: remove every area)
//   32 part index u8         33 part count u8
//   34 n_areas u8            35 total vertices u16
//   37 S[part * 202 .. ] (202 bytes, the last part the rest)
// every frame ends with
//   tag[16] = HMAC-SHA256(K_w, "ORUN-LORA-CMD-v1" || all bytes before)[0..16)
//
// RESULT (Tracker -> gateway -> sender), 56 bytes:
//   0  version 0x02          1  type 0x08
//   2  outcome (ConfigMutationOutcome; 7 = STALE)    3 resource
//   4  device id u64         12 key epoch u32        16 command id u32
//   20 token of the resource now: incarnation u64, 28 revision u32
//   resource 1: 32 tracking interval s u32, 36 ble window s u16,
//               38 service mode u8, 39 requested services u8
//   resource 2: 32 state (0 clear, 1 configured, 2 unavailable),
//               33 areas u8, 34 vertices u16
//   40 tag[16] = HMAC-SHA256(K_w, "ORUN-LORA-RES-v1" || bytes[0..40))[0..16)
//
// backend/lora_command.py and the app build/read the same bytes; shared
// vectors pin them.

#include <stddef.h>
#include <stdint.h>

#include "config_mutation.h"
#include "device_storage_role.h"
#include "geofence_geometry.h"
#include "geofence_mutation.h"
#include "geofence_store.h"
#include "security_store.h"

namespace orun_tlp {

namespace lora_command {
constexpr uint8_t kVersion = 0x02;
constexpr uint8_t kCommandType = 0x07;
constexpr uint8_t kResultType = 0x08;
constexpr uint8_t kResourceSettings = 1;
constexpr uint8_t kResourceAreas = 2;
constexpr size_t kTagSize = 16;
constexpr size_t kMaxFrameSize = 255;  // one LoRa packet
// settings
constexpr size_t kHeaderSize = 33;
constexpr size_t kEntrySize = 5;
constexpr size_t kMaxEntries = ConfigMutationOwner::kMaxBatch;
constexpr size_t kMinCommandSize = kHeaderSize + kEntrySize + kTagSize;           // 54
constexpr size_t kMaxCommandSize = kHeaderSize + kMaxEntries * kEntrySize + kTagSize;  // 89
// areas
constexpr size_t kAreaHeaderSize = 37;
constexpr size_t kAreaChunk = kMaxFrameSize - kAreaHeaderSize - kTagSize;  // 202
constexpr size_t kMaxAreaSet = 1 + geofence_format::kMaximumAreas +
                               8 * geofence_format::kMaximumTotalVertices;  // 521
constexpr uint8_t kMaxAreaParts = (kMaxAreaSet + kAreaChunk - 1) / kAreaChunk;  // 3
constexpr size_t kMinFrameSize = kAreaHeaderSize + 1 + kTagSize;  // smallest valid frame (54)
// result
constexpr size_t kResultSize = 56;
constexpr size_t kResultBodySize = kResultSize - kTagSize;  // 40
constexpr char kCommandDomain[] = "ORUN-LORA-CMD-v1";
constexpr char kResultDomain[] = "ORUN-LORA-RES-v1";
constexpr size_t kDomainSize = sizeof(kCommandDomain) - 1U;  // 16
static_assert(sizeof(kResultDomain) - 1U == kDomainSize, "domain size");
static_assert(kMinCommandSize == kMinFrameSize, "smallest frame");

/** Does this look like a LoRa command frame (version/type only)? */
inline bool isCommandFrame(const uint8_t* bytes, size_t size) {
  return bytes != nullptr && size >= 2 && bytes[0] == kVersion && bytes[1] == kCommandType;
}

/** Size of the area set S for these totals. */
constexpr size_t areaSetSize(uint8_t areas, uint16_t vertices) {
  return 1U + areas + 8U * vertices;
}

/**
 * Shape-only check of a command frame (no signature): the size and the
 * resource's own counts agree. What a Gateway may check, and the first thing
 * the Tracker checks.
 */
bool wellFormed(const uint8_t* frame, size_t size);
}  // namespace lora_command

enum class LoraCommandAdmission : uint8_t {
  kNotForMe = 0,   // another device's command, or not a command: ignore quietly
  kRejected = 1,   // malformed, wrong key/epoch, bad tag: ignored, never answered
  kBusy = 2,       // the one mutation slot is held: the gateway sends it again later
  kPending = 3,    // a signed result will be takeable (takeResult)
  kPartial = 4,    // one part of an area list kept; waiting for the others
};

// Loop-owned, like BleWriteService. One command at a time.
class LoraCommandService {
 public:
  LoraCommandService(const SecurityStore& security, ConfigStore& store,
                     ConfigMutationOwner& mutations, GeofenceStore& fences,
                     GeofenceMutationOwner& fence_mutations)
      : security_(security), store_(store), mutations_(mutations), fences_(fences),
        fence_mutations_(fence_mutations) {}

  // The storage role a setting must fit (settings_table.h roles).
  void setRole(StorageRole role) { role_ = role; }

  LoraCommandAdmission submit(const uint8_t* frame, size_t size);

  // Call after the mutation owners' poll(). True once per answered command,
  // with the signed RESULT bytes.
  bool takeResult(uint8_t (&out)[lora_command::kResultSize]);

  bool resultOwed() const { return owed_ != Owed::kNone || immediate_; }

 private:
  enum class Owed : uint8_t { kNone, kSettings, kAreas };

  bool writeKey(uint8_t (&key)[32]) const;
  bool tag(const char* domain, const uint8_t* body, size_t len,
           uint8_t (&out)[lora_command::kTagSize]) const;
  void buildSettingsResult(ConfigMutationOutcome outcome, bool token_valid,
                           const config_format::StateToken& token,
                           const config_format::Config& config);
  void buildAreasResult(ConfigMutationOutcome outcome);
  void sign(uint8_t resource);
  void remember();
  LoraCommandAdmission settings(const uint8_t* frame, size_t size, uint32_t command_id);
  LoraCommandAdmission areas(const uint8_t* frame, size_t size, uint32_t command_id);
  LoraCommandAdmission submitAreas();

  const SecurityStore& security_;
  ConfigStore& store_;
  ConfigMutationOwner& mutations_;
  GeofenceStore& fences_;
  GeofenceMutationOwner& fence_mutations_;
  StorageRole role_ = StorageRole::kUnassigned;
  Owed owed_ = Owed::kNone;
  bool immediate_ = false;  // refusal / repeated answer already built
  uint32_t command_id_ = 0;
  // The last answered command, re-sent as is if it comes again (its RESULT
  // was lost on the way back). RAM only: after a restart the replay is
  // answered STALE, which carries the same truth.
  bool last_valid_ = false;
  uint32_t last_command_id_ = 0;
  uint8_t result_[lora_command::kResultSize]{};
  // An area list being collected from its parts (one command at a time).
  bool area_active_ = false;
  uint32_t area_command_id_ = 0;
  uint8_t area_parts_ = 0;
  uint8_t area_have_ = 0;  // bit per received part
  uint8_t area_count_ = 0;
  uint16_t area_vertices_ = 0;
  uint64_t area_token_incarnation_ = 0;
  uint32_t area_token_revision_ = 0;
  uint8_t area_set_[lora_command::kMaxAreaSet]{};
  GeoPointE7 area_points_[geofence_format::kMaximumTotalVertices];
  GeofencePolygonView area_polygons_[geofence_format::kMaximumAreas];
};

}  // namespace orun_tlp
