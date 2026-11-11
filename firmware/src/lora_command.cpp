// 6: account-signed settings over LoRa (lora_command.h).
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
  constexpr size_t kMaxBody = lora_command::kMaxCommandSize - kTagSize;
  if (body == nullptr || len > kMaxBody) return false;
  uint8_t key[32]{};
  if (!writeKey(key)) return false;
  uint8_t input[kDomainSize + kMaxBody]{};
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

void LoraCommandService::buildResult(ConfigMutationOutcome outcome, bool token_valid,
                                     const config_format::StateToken& token,
                                     const config_format::Config& config,
                                     uint8_t (&out)[lora_command::kResultSize]) const {
  memset(out, 0, sizeof(out));
  out[0] = lora_command::kVersion;
  out[1] = lora_command::kResultType;
  out[2] = static_cast<uint8_t>(outcome);
  out[3] = lora_command::kResourceSettings;
  putBe64(out + 4, security_.credential_.device_identity);
  putBe32(out + 12, security_.credential_.key_epoch);
  putBe32(out + 16, command_id_);
  if (token_valid) {
    putBe64(out + 20, token.incarnation);
    putBe32(out + 28, token.revision);
  }
  putBe32(out + 32, config.tracking_interval_seconds);
  putBe16(out + 36, config.ble_window_seconds);
  out[38] = config.service_mode;
  out[39] = config.requested_services;
  uint8_t t[kTagSize]{};
  if (tag(lora_command::kResultDomain, out, lora_command::kResultBodySize, t))
    memcpy(out + lora_command::kResultBodySize, t, kTagSize);
}

LoraCommandAdmission LoraCommandService::submit(const uint8_t* frame, size_t size) {
  using namespace lora_command;
  if (!isCommandFrame(frame, size)) return LoraCommandAdmission::kNotForMe;
  if (size < 12U) return LoraCommandAdmission::kRejected;
  if (!security_.ready() || security_.state() != SecurityState::kProvisioned ||
      getBe64(frame + 4) != security_.credential_.device_identity)
    return LoraCommandAdmission::kNotForMe;
  if (size < kMinCommandSize) return LoraCommandAdmission::kRejected;
  const size_t n = frame[32];
  if (frame[2] != 0U || frame[3] != kResourceSettings || n == 0U || n > kMaxEntries ||
      size != kHeaderSize + n * kEntrySize + kTagSize ||
      getBe32(frame + 12) != security_.credential_.key_epoch)
    return LoraCommandAdmission::kRejected;
  const size_t body_len = size - kTagSize;
  uint8_t expected[kTagSize]{};
  const bool authentic = tag(kCommandDomain, frame, body_len, expected) &&
                         constantTimeEqual(expected, frame + body_len, kTagSize);
  secureZero(expected, sizeof(expected));
  // A forged or damaged frame gets no answer at all (nothing to amplify).
  if (!authentic) return LoraCommandAdmission::kRejected;
  if (owed_ || immediate_) return LoraCommandAdmission::kBusy;

  const uint32_t command_id = getBe32(frame + 16);
  // The same signed command again: its answer was lost; send the same one.
  if (last_valid_ && command_id == last_command_id_ &&
      constantTimeEqual(last_command_tag_, frame + body_len, kTagSize)) {
    immediate_ = true;
    return LoraCommandAdmission::kPending;
  }

  command_id_ = command_id;
  memcpy(pending_tag_, frame + body_len, kTagSize);
  const ApplicationAccessContext access(ApplicationAccessChannel::kLoraAuthenticated);
  ConfigMutationRequest items[kMaxEntries] = {
      ConfigMutationRequest(ApplicationRequester::kLora, command_id, access,
                            ConfigMutationKind::kSetTrackingInterval, 0),
      ConfigMutationRequest(ApplicationRequester::kLora, command_id, access,
                            ConfigMutationKind::kSetTrackingInterval, 0),
      ConfigMutationRequest(ApplicationRequester::kLora, command_id, access,
                            ConfigMutationKind::kSetTrackingInterval, 0),
      ConfigMutationRequest(ApplicationRequester::kLora, command_id, access,
                            ConfigMutationKind::kSetTrackingInterval, 0),
      ConfigMutationRequest(ApplicationRequester::kLora, command_id, access,
                            ConfigMutationKind::kSetTrackingInterval, 0),
      ConfigMutationRequest(ApplicationRequester::kLora, command_id, access,
                            ConfigMutationKind::kSetTrackingInterval, 0),
      ConfigMutationRequest(ApplicationRequester::kLora, command_id, access,
                            ConfigMutationKind::kSetTrackingInterval, 0),
      ConfigMutationRequest(ApplicationRequester::kLora, command_id, access,
                            ConfigMutationKind::kSetTrackingInterval, 0),
  };
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
    buildResult(ConfigMutationOutcome::kInvalid, token_valid, token, store_.config(), result_);
    immediate_ = true;
    last_valid_ = true;
    last_command_id_ = command_id;
    memcpy(last_command_tag_, pending_tag_, kTagSize);
    return LoraCommandAdmission::kPending;
  }

  const config_format::StateToken want(getBe64(frame + 20), getBe32(frame + 28));
  switch (mutations_.submitBatch(items, n, &want)) {
    case ConfigMutationSubmitResult::kAccepted:
      owed_ = true;
      return LoraCommandAdmission::kPending;
    case ConfigMutationSubmitResult::kBusy:
      return LoraCommandAdmission::kBusy;
    case ConfigMutationSubmitResult::kRejected:
    default:
      return LoraCommandAdmission::kRejected;
  }
}

bool LoraCommandService::takeResult(uint8_t (&out)[lora_command::kResultSize]) {
  if (immediate_) {
    immediate_ = false;
    memcpy(out, result_, sizeof(out));
    return true;
  }
  if (!owed_) return false;
  ConfigMutationResult done;
  if (!mutations_.takeResult(ApplicationRequester::kLora, done)) return false;
  owed_ = false;
  buildResult(done.outcome, done.token_valid, done.token, done.config, result_);
  last_valid_ = true;
  last_command_id_ = command_id_;
  memcpy(last_command_tag_, pending_tag_, lora_command::kTagSize);
  memcpy(out, result_, sizeof(out));
  return true;
}

}  // namespace orun_tlp
