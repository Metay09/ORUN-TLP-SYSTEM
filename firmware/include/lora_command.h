#pragma once

// 6 (owner decision 2026-11-11): settings — later the area list — over LoRa.
//
// The account server signs a command with the same per-Tracker write key the
// phones use over Bluetooth (ble_write_auth.h: K_w from K_root, never K_root
// itself). The gateway only carries the bytes; it holds no authority. The
// command is bound to the Tracker's current saved state token: once applied
// the token moves, so a replayed or overtaken command changes nothing and is
// answered STALE with the current token (the server signs again against it).
// All settings of one command are written in ONE save, all or none. Radio
// settings are never accepted here (settings_table.h radio rows).
//
// COMMAND (gateway -> Tracker), big-endian:
//   0  version 0x02          1  type 0x07
//   2  flags (0)             3  resource (1 = settings)
//   4  target device id u64  12 key epoch u32      16 command id u32
//   20 expected token: incarnation u64, 28 revision u32
//   32 n (1..8)              33 n x { setting id u8, value u32 }
//   33+5n  tag[16] = HMAC-SHA256(K_w, "ORUN-LORA-CMD-v1" || bytes[0..33+5n))[0..16)
//
// RESULT (Tracker -> gateway -> server), 56 bytes:
//   0  version 0x02          1  type 0x08
//   2  outcome (ConfigMutationOutcome; 7 = STALE)    3 resource
//   4  device id u64         12 key epoch u32        16 command id u32
//   20 token now: incarnation u64, 28 revision u32
//   32 tracking interval s u32   36 ble window s u16
//   38 service mode u8           39 requested services u8
//   40 tag[16] = HMAC-SHA256(K_w, "ORUN-LORA-RES-v1" || bytes[0..40))[0..16)
//
// backend/lora_command.py builds/reads the same bytes; a shared vector pins it.

#include <stddef.h>
#include <stdint.h>

#include "config_mutation.h"
#include "device_storage_role.h"
#include "security_store.h"

namespace orun_tlp {

namespace lora_command {
constexpr uint8_t kVersion = 0x02;
constexpr uint8_t kCommandType = 0x07;
constexpr uint8_t kResultType = 0x08;
constexpr uint8_t kResourceSettings = 1;
constexpr size_t kHeaderSize = 33;
constexpr size_t kEntrySize = 5;
constexpr size_t kMaxEntries = ConfigMutationOwner::kMaxBatch;
constexpr size_t kTagSize = 16;
constexpr size_t kMinCommandSize = kHeaderSize + kEntrySize + kTagSize;           // 54
constexpr size_t kMaxCommandSize = kHeaderSize + kMaxEntries * kEntrySize + kTagSize;  // 89
constexpr size_t kResultSize = 56;
constexpr size_t kResultBodySize = kResultSize - kTagSize;  // 40
constexpr char kCommandDomain[] = "ORUN-LORA-CMD-v1";
constexpr char kResultDomain[] = "ORUN-LORA-RES-v1";
constexpr size_t kDomainSize = sizeof(kCommandDomain) - 1U;  // 16
static_assert(sizeof(kResultDomain) - 1U == kDomainSize, "domain size");

/** Does this look like a LoRa command frame (version/type only)? */
inline bool isCommandFrame(const uint8_t* bytes, size_t size) {
  return bytes != nullptr && size >= 2 && bytes[0] == kVersion && bytes[1] == kCommandType;
}
}  // namespace lora_command

enum class LoraCommandAdmission : uint8_t {
  kNotForMe = 0,   // another device's command, or not a command: ignore quietly
  kRejected = 1,   // malformed, wrong key/epoch, bad tag: ignored, never answered
  kBusy = 2,       // the one mutation slot is held: the gateway sends it again later
  kPending = 3,    // a signed result will be takeable (takeResult)
};

// Loop-owned, like BleWriteService. One command at a time.
class LoraCommandService {
 public:
  LoraCommandService(const SecurityStore& security, ConfigStore& store,
                     ConfigMutationOwner& mutations)
      : security_(security), store_(store), mutations_(mutations) {}

  // The storage role a setting must fit (settings_table.h roles).
  void setRole(StorageRole role) { role_ = role; }

  LoraCommandAdmission submit(const uint8_t* frame, size_t size);

  // Call after ConfigMutationOwner::poll(). True once per submitted command,
  // with the signed RESULT bytes.
  bool takeResult(uint8_t (&out)[lora_command::kResultSize]);

  bool resultOwed() const { return owed_ || immediate_; }

 private:
  bool writeKey(uint8_t (&key)[32]) const;
  bool tag(const char* domain, const uint8_t* body, size_t len,
           uint8_t (&out)[lora_command::kTagSize]) const;
  void buildResult(ConfigMutationOutcome outcome, bool token_valid,
                   const config_format::StateToken& token,
                   const config_format::Config& config,
                   uint8_t (&out)[lora_command::kResultSize]) const;

  const SecurityStore& security_;
  ConfigStore& store_;
  ConfigMutationOwner& mutations_;
  StorageRole role_ = StorageRole::kUnassigned;
  bool owed_ = false;       // waiting for the mutation owner
  bool immediate_ = false;  // refusal already built
  uint32_t command_id_ = 0;
  // The last answered command, re-sent as is if the same signed command comes
  // again (its RESULT was lost on the way back). RAM only: after a restart
  // the replay is answered STALE, which carries the same truth.
  bool last_valid_ = false;
  uint32_t last_command_id_ = 0;
  uint8_t last_command_tag_[lora_command::kTagSize]{};
  uint8_t pending_tag_[lora_command::kTagSize]{};
  uint8_t result_[lora_command::kResultSize]{};
};

}  // namespace orun_tlp
