// 6: account-signed settings and area lists over LoRa (lora_command.h).
#include "lora_command.h"

#include <string.h>

#include "ble_write_auth.h"
#include "custody_ack_primitives.h"
#include "settings_table.h"

namespace orun_tlp {

namespace {

using lora_command::kDomainSize;
using lora_command::kTagSize;

void secureZero(void* p, size_t n) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  while (n-- != 0U) *v++ = 0U;
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t diff = 0U;
  for (size_t i = 0U; i < n; ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
  return diff == 0U;
}

uint64_t getBe64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}
uint32_t getBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint16_t getBe16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
void putBe64(uint8_t* p, uint64_t v) {
  for (int i = 7; i >= 0; --i) { p[i] = static_cast<uint8_t>(v); v >>= 8; }
}
void putBe32(uint8_t* p, uint32_t v) {
  for (int i = 3; i >= 0; --i) { p[i] = static_cast<uint8_t>(v); v >>= 8; }
}
void putBe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

}  // namespace


bool LoraCommandService::writeKey(uint8_t (&key)[32]) const {
  // Exactly the Bluetooth write key (ble_write_auth.h): one key per Tracker
  // and epoch for every account-authenticated door.
  if (!security_.ready() || security_.state() != SecurityState::kProvisioned ||
      security_.credential_.device_identity == 0U)
    return false;
  constexpr size_t kLabel = ble_write_auth::kLabelSize;
  uint8_t salt[security_format::kCredentialIdSize]{};
  uint8_t root[security_format::kKRootSize]{};
  uint8_t info[kLabel + 8U + 4U]{};
  memcpy(salt, security_.credential_.credential_id, sizeof(salt));
  memcpy(root, security_.credential_.k_root, sizeof(root));
  memcpy(info, ble_write_auth::kKeyInfoLabel, kLabel);
  putBe64(info + kLabel, security_.credential_.device_identity);
  putBe32(info + kLabel + 8U, security_.credential_.key_epoch);
  const bool ok = custody_crypto::hkdfSha256Bounded(salt, root, info, sizeof(info), key);
  secureZero(root, sizeof(root));
  secureZero(salt, sizeof(salt));
  return ok;
}

bool LoraCommandService::tag(const char* domain, const uint8_t* body, size_t len,
                             uint8_t (&out)[kTagSize]) const {
  constexpr size_t kMaxBody = lora_command::kMaxFrameSize - kTagSize;
  if (body == nullptr || len > kMaxBody) return false;
  uint8_t key[32]{};
  if (!writeKey(key)) return false;
  // Loop-owned: kept off the loop stack (frames are up to 255 bytes).
  static uint8_t input[kDomainSize + kMaxBody];
  memcpy(input, domain, kDomainSize);
  memcpy(input + kDomainSize, body, len);
  uint8_t mac[32]{};
  const bool ok = custody_crypto::hmacSha256Bounded(key, input, kDomainSize + len, mac);
  secureZero(key, sizeof(key));
  secureZero(input, sizeof(input));
  if (ok) memcpy(out, mac, kTagSize);
  secureZero(mac, sizeof(mac));
  return ok;
}

void LoraCommandService::sign(uint8_t resource) {
  result_[0] = lora_command::kVersion;
  result_[1] = lora_command::kResultType;
  result_[3] = resource;
  putBe64(result_ + 4, security_.credential_.device_identity);
  putBe32(result_ + 12, security_.credential_.key_epoch);
  putBe32(result_ + 16, command_id_);
  uint8_t t[kTagSize]{};
  if (tag(lora_command::kResultDomain, result_, lora_command::kResultBodySize, t))
    memcpy(result_ + lora_command::kResultBodySize, t, kTagSize);
}

void LoraCommandService::buildSettingsResult(ConfigMutationOutcome outcome, bool token_valid,
                                             const config_format::StateToken& token,
                                             const config_format::Config& config) {
  memset(result_, 0, sizeof(result_));
  result_[2] = static_cast<uint8_t>(outcome);
  if (token_valid) {
    putBe64(result_ + 20, token.incarnation);
    putBe32(result_ + 28, token.revision);
  }
  putBe32(result_ + 32, config.tracking_interval_seconds);
  putBe16(result_ + 36, config.ble_window_seconds);
  result_[38] = config.service_mode;
  result_[39] = config.requested_services;
  sign(lora_command::kResourceSettings);
}

void LoraCommandService::buildAreasResult(ConfigMutationOutcome outcome) {
  memset(result_, 0, sizeof(result_));
  result_[2] = static_cast<uint8_t>(outcome);
  geofence_format::StateToken token;
  if (fences_.stateToken(token)) {
    putBe64(result_ + 20, token.incarnation);
    putBe32(result_ + 28, token.revision);
  }
  const GeofenceResourceState state = fences_.resourceState();
  result_[32] = state == GeofenceResourceState::kClear        ? 0U
                : state == GeofenceResourceState::kConfigured ? 1U
                                                              : 2U;
  result_[33] = fences_.areaCount();
  putBe16(result_ + 34, fences_.totalVertexCount());
  sign(lora_command::kResourceAreas);
}

void LoraCommandService::remember() {
  last_valid_ = true;
  last_command_id_ = command_id_;
}

LoraCommandAdmission LoraCommandService::submit(const uint8_t* frame, size_t size) {
  using namespace lora_command;
  if (!isCommandFrame(frame, size)) return LoraCommandAdmission::kNotForMe;
  if (size < 12U) return LoraCommandAdmission::kRejected;
  if (!security_.ready() || security_.state() != SecurityState::kProvisioned ||
      getBe64(frame + 4) != security_.credential_.device_identity)
    return LoraCommandAdmission::kNotForMe;
  if (!wellFormed(frame, size) || getBe32(frame + 12) != security_.credential_.key_epoch)
    return LoraCommandAdmission::kRejected;
  const size_t body_len = size - kTagSize;
  uint8_t expected[kTagSize]{};
  const bool authentic = tag(kCommandDomain, frame, body_len, expected) &&
                         constantTimeEqual(expected, frame + body_len, kTagSize);
  secureZero(expected, sizeof(expected));
  // A forged or damaged frame gets no answer at all (nothing to amplify).
  if (!authentic) return LoraCommandAdmission::kRejected;
  if (owed_ != Owed::kNone || immediate_) return LoraCommandAdmission::kBusy;

  const uint32_t command_id = getBe32(frame + 16);
  // The same signed command again (any part): its answer was lost; resend it.
  if (last_valid_ && command_id == last_command_id_) {
    immediate_ = true;
    return LoraCommandAdmission::kPending;
  }
  return frame[3] == kResourceSettings ? settings(frame, size, command_id)
                                       : areas(frame, size, command_id);
}

LoraCommandAdmission LoraCommandService::settings(const uint8_t* frame, size_t size,
                                                  uint32_t command_id) {
  using namespace lora_command;
  (void)size;
  command_id_ = command_id;
  const size_t n = frame[32];
  const ApplicationAccessContext access(ApplicationAccessChannel::kLoraAuthenticated);
  const ConfigMutationRequest blank(ApplicationRequester::kLora, command_id, access,
                                    ConfigMutationKind::kSetTrackingInterval, 0);
  ConfigMutationRequest items[kMaxEntries] = {blank, blank, blank, blank,
                                              blank, blank, blank, blank};
  static_assert(kMaxEntries == 8, "items initializer");
  bool invalid = false;
  uint8_t seen = 0;  // each setting at most once per command
  for (size_t i = 0; i < n && !invalid; ++i) {
    const uint8_t* e = frame + kHeaderSize + i * kEntrySize;
    const SettingRow* row = findSetting(e[0]);
    invalid = row == nullptr || e[0] >= 8U || (seen & (1U << e[0])) != 0U ||
              !settingAllowed(*row, kSettingDoorLora, role_) ||
              !settingRequest(*row, ApplicationRequester::kLora, command_id, access,
                              getBe32(e + 1), &items[i]);
    if (!invalid) seen = static_cast<uint8_t>(seen | (1U << e[0]));
    if (!invalid && row->kind == ConfigMutationKind::kSetServiceIntent) {
      // Y2a: a device keeps its role, whatever door the intent comes from.
      const StorageRole asked =
          storageRoleForIntent(items[i].service_mode, items[i].requested_services);
      invalid = asked != StorageRole::kUnassigned && asked != role_;
    }
  }
  if (invalid) {
    config_format::StateToken token;
    const bool token_valid = store_.stateToken(token);
    buildSettingsResult(ConfigMutationOutcome::kInvalid, token_valid, token, store_.config());
    immediate_ = true;
    remember();
    return LoraCommandAdmission::kPending;
  }

  const config_format::StateToken want(getBe64(frame + 20), getBe32(frame + 28));
  switch (mutations_.submitBatch(items, n, &want)) {
    case ConfigMutationSubmitResult::kAccepted:
      owed_ = Owed::kSettings;
      return LoraCommandAdmission::kPending;
    case ConfigMutationSubmitResult::kBusy:
      return LoraCommandAdmission::kBusy;
    case ConfigMutationSubmitResult::kRejected:
    default:
      return LoraCommandAdmission::kRejected;
  }
}

LoraCommandAdmission LoraCommandService::areas(const uint8_t* frame, size_t size,
                                               uint32_t command_id) {
  using namespace lora_command;
  const uint8_t part = frame[32], parts = frame[33];
  const uint64_t inc = getBe64(frame + 20);
  const uint32_t rev = getBe32(frame + 28);
  if (!area_active_ || area_command_id_ != command_id) {
    // A new list starts (any earlier unfinished one is dropped).
    area_active_ = true;
    area_command_id_ = command_id;
    area_parts_ = parts;
    area_have_ = 0;
    area_count_ = frame[34];
    area_vertices_ = getBe16(frame + 35);
    area_token_incarnation_ = inc;
    area_token_revision_ = rev;
  } else if (parts != area_parts_ || frame[34] != area_count_ ||
             getBe16(frame + 35) != area_vertices_ || inc != area_token_incarnation_ ||
             rev != area_token_revision_) {
    return LoraCommandAdmission::kRejected;  // signed parts of one command never disagree
  }
  memcpy(area_set_ + size_t(part) * kAreaChunk, frame + kAreaHeaderSize,
         size - kAreaHeaderSize - kTagSize);
  area_have_ = static_cast<uint8_t>(area_have_ | (1U << part));
  if (area_have_ != static_cast<uint8_t>((1U << area_parts_) - 1U))
    return LoraCommandAdmission::kPartial;
  return submitAreas();
}

LoraCommandAdmission LoraCommandService::submitAreas() {
  using namespace lora_command;
  command_id_ = area_command_id_;
  const ApplicationAccessContext access(ApplicationAccessChannel::kLoraAuthenticated);
  const geofence_format::StateToken want(area_token_incarnation_, area_token_revision_);
  // S: n_areas, the vertex count of each area, then every vertex.
  const uint8_t n = area_set_[0];
  uint16_t sum = 0;
  bool invalid = role_ != StorageRole::kTracker || n != area_count_;
  for (uint8_t a = 0; a < n && !invalid; ++a) {
    const uint8_t count = area_set_[1 + a];
    invalid = count == 0U || sum + count > geofence_format::kMaximumTotalVertices;
    if (!invalid) area_polygons_[a] = GeofencePolygonView(area_points_ + sum, count);
    sum = static_cast<uint16_t>(sum + count);
  }
  invalid = invalid || sum != area_vertices_;
  if (invalid) {
    area_active_ = false;
    buildAreasResult(ConfigMutationOutcome::kInvalid);
    immediate_ = true;
    remember();
    return LoraCommandAdmission::kPending;
  }
  const uint8_t* v = area_set_ + 1 + n;
  for (uint16_t i = 0; i < sum; ++i) {
    area_points_[i] = GeoPointE7(static_cast<int32_t>(getBe32(v + 8U * i)),
                                 static_cast<int32_t>(getBe32(v + 8U * i + 4U)));
  }
  const GeofenceAreaSetView set(area_polygons_, n);
  const ConfigMutationSubmitResult r =
      n == 0U ? fence_mutations_.submitClearIf(ApplicationRequester::kLora, command_id_, access, want)
              : fence_mutations_.submitReplaceIf(ApplicationRequester::kLora, command_id_, access,
                                                 set, want);
  switch (r) {
    case ConfigMutationSubmitResult::kAccepted:
      area_active_ = false;
      owed_ = Owed::kAreas;
      return LoraCommandAdmission::kPending;
    case ConfigMutationSubmitResult::kBusy:
      // Kept: the next copy of part 0 completes it again and submits.
      area_have_ = static_cast<uint8_t>(area_have_ & ~1U);
      return LoraCommandAdmission::kBusy;
    case ConfigMutationSubmitResult::kRejected:
    default:
      area_active_ = false;
      return LoraCommandAdmission::kRejected;
  }
}

bool LoraCommandService::takeResult(uint8_t (&out)[lora_command::kResultSize]) {
  if (immediate_) {
    immediate_ = false;
    memcpy(out, result_, sizeof(out));
    return true;
  }
  if (owed_ == Owed::kSettings) {
    ConfigMutationResult done;
    if (!mutations_.takeResult(ApplicationRequester::kLora, done)) return false;
    buildSettingsResult(done.outcome, done.token_valid, done.token, done.config);
  } else if (owed_ == Owed::kAreas) {
    GeofenceMutationResult done;
    if (!fence_mutations_.takeResult(ApplicationRequester::kLora, done)) return false;
    buildAreasResult(done.outcome);
  } else {
    return false;
  }
  owed_ = Owed::kNone;
  remember();
  memcpy(out, result_, sizeof(out));
  return true;
}

}  // namespace orun_tlp
