// 6: account-signed settings over LoRa (lora_command.h), host end to end.
//
// LoraCommandService (real SecurityStore credential, OpenSSL crypto port)
// -> ConfigMutationOwner::submitBatch -> ConfigStore (fake NOR). The server
// side is computed independently here with OpenSSL, and against the vector
// shared with backend/lora_command.py (backend/tests/test_lora_command.py).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>

#include "ble_write_auth.h"
#include "config_mutation.h"
#include "config_store.h"
#include "device_identity.h"
#include "lora_command.h"
#include "security_store.h"
#include "storage_config.h"

using namespace orun_tlp;
namespace lc = orun_tlp::lora_command;

namespace {

constexpr uint64_t kDevice = UINT64_C(0x0E8ADE7E71531AA3);
constexpr uint32_t kEpoch = 1U;
constexpr uint64_t kIncarnation = 0x0BADC0FFEE123456ULL;
constexpr size_t kConfigRegion =
    storage_config::kPageSize * storage_config::kFutureConfigRegionPages;

// Shared vector: credential 10..1F, root 40..5F; command id 01020304,
// token (0BADC0FFEE123456, 1); INTERVAL 900, BLE_WINDOW 60.
const char kVectorCommand[] =
    "020700010E8ADE7E71531AA300000001010203040BADC0FFEE12345600000001020100000384030000003C"
    "EDD9ABC7127362652C2CE8D98CDCD396";

std::vector<uint8_t> unhex(const char* text) {
  std::vector<uint8_t> out;
  for (size_t i = 0; text[i] != '\0' && text[i + 1] != '\0'; i += 2) {
    unsigned v = 0;
    sscanf(text + i, "%2x", &v);
    out.push_back(static_cast<uint8_t>(v));
  }
  return out;
}

class NorFlash : public FlashBackend {
 public:
  explicit NorFlash(size_t size, uint32_t pages) : bytes(size, 0xFF), pages_(pages) {}
  std::vector<uint8_t> bytes;
  bool begin() override { return true; }
  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || offset > bytes.size() || size > bytes.size() - offset) return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }
  FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
    if (data == nullptr || (offset & 3U) != 0 || (size & 3U) != 0 || offset > bytes.size() ||
        size > bytes.size() - offset)
      return FlashOpResult::kFailed;
    const auto* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) bytes[offset + i] &= src[i];
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t page) override {
    if (page >= pages_) return FlashOpResult::kFailed;
    memset(bytes.data() + size_t(page) * storage_config::kPageSize, 0xFF,
           storage_config::kPageSize);
    return FlashOpResult::kDone;
  }

 private:
  uint32_t pages_;
};

class FixedIncarnation : public ConfigIncarnationSource {
 public:
  bool generate(uint64_t& incarnation) override {
    incarnation = kIncarnation;
    return true;
  }
};

// ---- the server, independently ---------------------------------------------
std::vector<uint8_t> writeKey() {
  uint8_t salt[16], root[32];
  for (unsigned i = 0; i < 16; ++i) salt[i] = static_cast<uint8_t>(0x10 + i);
  for (unsigned i = 0; i < 32; ++i) root[i] = static_cast<uint8_t>(0x40 + i);
  std::vector<uint8_t> info(ble_write_auth::kKeyInfoLabel, ble_write_auth::kKeyInfoLabel + 21);
  for (int i = 7; i >= 0; --i) info.push_back(static_cast<uint8_t>(kDevice >> (8 * i)));
  for (int i = 3; i >= 0; --i) info.push_back(static_cast<uint8_t>(kEpoch >> (8 * i)));
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  std::vector<uint8_t> key(32);
  size_t n = key.size();
  assert(ctx && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1 &&
         EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, 16) == 1 &&
         EVP_PKEY_CTX_set1_hkdf_key(ctx, root, 32) == 1 &&
         EVP_PKEY_CTX_add1_hkdf_info(ctx, info.data(), info.size()) == 1 &&
         EVP_PKEY_derive(ctx, key.data(), &n) == 1 && n == 32);
  EVP_PKEY_CTX_free(ctx);
  return key;
}

std::vector<uint8_t> mac(const char* domain, const uint8_t* body, size_t len) {
  std::vector<uint8_t> input(domain, domain + 16);
  input.insert(input.end(), body, body + len);
  const std::vector<uint8_t> key = writeKey();
  uint8_t out[32];
  unsigned n = 0;
  assert(HMAC(EVP_sha256(), key.data(), 32, input.data(), input.size(), out, &n) && n == 32);
  return std::vector<uint8_t>(out, out + 16);
}

void put(std::vector<uint8_t>& v, uint64_t x, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
uint64_t get(const uint8_t* p, int bytes) {
  uint64_t v = 0;
  for (int i = 0; i < bytes; ++i) v = (v << 8) | p[i];
  return v;
}

struct Entry {
  uint8_t id;
  uint32_t value;
};

std::vector<uint8_t> command(uint32_t id, config_format::StateToken token,
                             std::vector<Entry> entries, uint64_t device = kDevice,
                             uint32_t epoch = kEpoch) {
  std::vector<uint8_t> f = {lc::kVersion, lc::kCommandType, 0, lc::kResourceSettings};
  put(f, device, 8);
  put(f, epoch, 4);
  put(f, id, 4);
  put(f, token.incarnation, 8);
  put(f, token.revision, 4);
  f.push_back(static_cast<uint8_t>(entries.size()));
  for (const Entry& e : entries) {
    f.push_back(e.id);
    put(f, e.value, 4);
  }
  const std::vector<uint8_t> t = mac(lc::kCommandDomain, f.data(), f.size());
  f.insert(f.end(), t.begin(), t.end());
  return f;
}

// ---- a Tracker -------------------------------------------------------------
struct Tracker {
  NorFlash sec_flash{8192U, 2U};
  NorFlash cfg_flash{kConfigRegion, storage_config::kFutureConfigRegionPages};
  FixedIncarnation rng;
  SecurityStore security{sec_flash, sec_flash};
  ConfigStore store{cfg_flash, &rng};
  ConfigMutationOwner mutations{store};
  LoraCommandService service{security, store, mutations};

  Tracker() {
    assert(store.begin());
    assert(security.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
    uint8_t id[16], root[32];
    for (unsigned i = 0; i < 16; ++i) id[i] = static_cast<uint8_t>(0x10 + i);
    for (unsigned i = 0; i < 32; ++i) root[i] = static_cast<uint8_t>(0x40 + i);
    assert(security.commitCredential(id, kEpoch, root));
    for (int i = 0; i < 3000 && security.busy(); ++i) security.poll();
    bool ok = false;
    assert(security.takeCommitResult(ok) && ok);
    service.setRole(StorageRole::kTracker);
  }

  config_format::StateToken token() {
    config_format::StateToken t;
    assert(store.stateToken(t));
    return t;
  }

  // Submits and runs the loop until the signed result; checks its tag.
  std::vector<uint8_t> run(const std::vector<uint8_t>& frame,
                           LoraCommandAdmission want = LoraCommandAdmission::kPending) {
    const LoraCommandAdmission got = service.submit(frame.data(), frame.size());
    if (got != want) fprintf(stderr, "admission %u want %u size %zu b2 %u\n", unsigned(got), unsigned(want), frame.size(), unsigned(frame.size() > 2 ? frame[2] : 0));
    assert(got == want);
    if (want != LoraCommandAdmission::kPending) return {};
    uint8_t out[lc::kResultSize];
    for (int i = 0; i < 400; ++i) {
      store.poll();
      (void)mutations.poll();
      if (service.takeResult(out)) {
        std::vector<uint8_t> r(out, out + sizeof(out));
        assert(r[0] == lc::kVersion && r[1] == lc::kResultType);
        assert(get(r.data() + 4, 8) == kDevice && get(r.data() + 12, 4) == kEpoch);
        const std::vector<uint8_t> t = mac(lc::kResultDomain, r.data(), lc::kResultBodySize);
        assert(memcmp(t.data(), r.data() + lc::kResultBodySize, 16) == 0);  // the Tracker said it
        return r;
      }
    }
    assert(false && "no result");
    return {};
  }
};

constexpr uint8_t kApplied = uint8_t(ConfigMutationOutcome::kApplied);
constexpr uint8_t kUnchanged = uint8_t(ConfigMutationOutcome::kUnchanged);
constexpr uint8_t kInvalid = uint8_t(ConfigMutationOutcome::kInvalid);
constexpr uint8_t kStale = uint8_t(ConfigMutationOutcome::kStale);

void testSharedVectorApplies() {
  Tracker t;
  // The vector was signed for the fresh store's token.
  const config_format::StateToken start = t.token();
  assert(start.incarnation == kIncarnation && start.revision == 1);
  const std::vector<uint8_t> v = unhex(kVectorCommand);
  assert(v == command(0x01020304, start, {{1, 900}, {3, 60}}));
  const std::vector<uint8_t> r = t.run(v);
  // Both settings in one save: the revision moves by exactly one.
  assert(r[2] == kApplied && get(r.data() + 16, 4) == 0x01020304);
  assert(get(r.data() + 20, 8) == kIncarnation && get(r.data() + 28, 4) == 2);
  assert(get(r.data() + 32, 4) == 900 && get(r.data() + 36, 2) == 60);
  assert(t.store.config().tracking_interval_seconds == 900);
  assert(t.store.config().ble_window_seconds == 60);
}

void testReplayChangesNothing() {
  Tracker t;
  const std::vector<uint8_t> first = command(7, t.token(), {{1, 600}});
  const std::vector<uint8_t> a = t.run(first);
  assert(a[2] == kApplied);
  // The same signed command again (its answer was lost): the same answer.
  const std::vector<uint8_t> b = t.run(first);
  assert(a == b);
  // Something else changed it since; an old command (any id) is STALE.
  assert(t.run(command(8, t.token(), {{1, 1200}}))[2] == kApplied);
  const std::vector<uint8_t> c = t.run(command(9, {kIncarnation, 2}, {{1, 600}}));
  assert(c[2] == kStale && get(c.data() + 28, 4) == 3 && get(c.data() + 32, 4) == 1200);
  assert(t.store.config().tracking_interval_seconds == 1200);
  // ...and the first command, replayed after another one, is STALE too.
  const std::vector<uint8_t> d = t.run(first);
  assert(d[2] == kStale && t.store.config().tracking_interval_seconds == 1200);
  // Same values as now: UNCHANGED, token not moved.
  const std::vector<uint8_t> e = t.run(command(10, t.token(), {{1, 1200}}));
  assert(e[2] == kUnchanged && get(e.data() + 28, 4) == 3);
}

void testAllOrNothing() {
  Tracker t;
  const uint32_t before = t.store.config().tracking_interval_seconds;
  // A valid interval with an inadmissible window: neither is written.
  const std::vector<uint8_t> r = t.run(command(11, t.token(), {{1, 900}, {3, 9999}}));
  assert(r[2] == kInvalid && t.store.config().tracking_interval_seconds == before);
  assert(t.token().revision == 1);
}

void testRefusals() {
  Tracker t;
  const config_format::StateToken now = t.token();
  // Not ours, not a command: ignored.
  std::vector<uint8_t> other = command(1, now, {{1, 900}}, kDevice + 1);
  assert(t.service.submit(other.data(), other.size()) == LoraCommandAdmission::kNotForMe);
  // Forged / damaged / wrong epoch / bad shape: never answered.
  std::vector<uint8_t> forged = command(1, now, {{1, 900}});
  forged.back() ^= 1;
  t.run(forged, LoraCommandAdmission::kRejected);
  std::vector<uint8_t> tampered = command(1, now, {{1, 900}});
  tampered[36] ^= 1;  // value changed after signing
  t.run(tampered, LoraCommandAdmission::kRejected);
  t.run(command(1, now, {{1, 900}}, kDevice, kEpoch + 1), LoraCommandAdmission::kRejected);
  std::vector<uint8_t> shortened = command(1, now, {{1, 900}});
  shortened.pop_back();
  t.run(shortened, LoraCommandAdmission::kRejected);
  // Signed but unusable: an answered INVALID, nothing written.
  assert(t.run(command(2, now, {{9, 1}}))[2] == kInvalid);           // no such setting
  assert(t.run(command(3, now, {{1, 900}, {1, 600}}))[2] == kInvalid);  // twice
  assert(t.run(command(4, now, {{1, 30}}))[2] == kInvalid);          // below the minimum
  // Y2a: a Gateway intent on a Tracker is the other role.
  assert(t.run(command(5, now, {{2, 1U | (0x04U << 8)}}))[2] == kInvalid);
  assert(t.token().revision == 1);
  // A Gateway takes no tracking interval over LoRa either.
  t.service.setRole(StorageRole::kGateway);
  assert(t.run(command(6, now, {{1, 900}}))[2] == kInvalid);
  assert(t.token().revision == 1);
}

void testBusy() {
  Tracker t;
  const ConfigMutationRequest usb(ApplicationRequester::kUsb, 77,
                                  defaultApplicationAccess(ApplicationRequester::kUsb),
                                  ConfigMutationKind::kSetTrackingInterval, 1200);
  assert(t.mutations.submit(usb) == ConfigMutationSubmitResult::kAccepted);
  const std::vector<uint8_t> f = command(1, t.token(), {{1, 900}});
  // The one slot is held: the gateway simply sends it again later.
  t.run(f, LoraCommandAdmission::kBusy);
  for (int i = 0; i < 400 && t.mutations.busy(); ++i) {
    t.store.poll();
    (void)t.mutations.poll();
    ConfigMutationResult done;
    (void)t.mutations.takeResult(ApplicationRequester::kUsb, done);
  }
  // By then the USB change moved the token: the old command is STALE.
  assert(t.run(f)[2] == kStale && t.store.config().tracking_interval_seconds == 1200);
}

}  // namespace

int main() {
  testSharedVectorApplies();
  testReplayChangesNothing();
  testAllOrNothing();
  testRefusals();
  testBusy();
  puts("6a LoRa settings command (vector, one save, replay, all-or-nothing, refusals, busy): PASS");
  return 0;
}
