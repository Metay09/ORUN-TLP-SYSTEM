// 4c-1b: authenticated Bluetooth configuration writes, host end to end.
//
// Real BleApplicationTransport -> BleWriteService -> BleWriteAuthenticator
// (real SecurityStore credential, OpenSSL crypto port) -> ConfigMutationOwner
// -> ConfigStore (fake NOR). The phone side is computed independently here
// with OpenSSL, against the vector shared with backend/orun_credentials.py
// and the app (edge/app/test/bleYazma.test.ts).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>

#include "application_request.h"
#include "ble_application_transport.h"
#include "ble_write_auth.h"
#include "ble_write_service.h"
#include "config_mutation.h"
#include "config_store.h"
#include "device_identity.h"
#include "security_store.h"
#include "storage_config.h"

using namespace orun_tlp;
namespace bat = orun_tlp::ble_app_transport;

namespace {

constexpr uint64_t kDevice = UINT64_C(0x0E8ADE7E71531AA3);
constexpr uint32_t kEpoch = 1U;
constexpr size_t kConfigRegion =
    storage_config::kPageSize * storage_config::kFutureConfigRegionPages;

// Shared vector (credential 10..1F, root 40..5F, challenge C0..CF).
const char kWriteKey[] =
    "75BF1BD3052DFA4CD5E9DB9DD084B98DB9622C2FABE44588FB58E02EC0952DD2";
const char kRequestTag[] = "79D0DB48C72ED2FF66C18AA77177D44A";
const char kResultBody[] = "0001B40000001E00010102000000";
const char kResultTag[] = "4CD68F40DDA649FBF6F160E046FC09C6";

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
    if (data == nullptr || offset > bytes.size() || size > bytes.size() - offset)
      return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }
  FlashOpResult program(uint32_t offset, const void* data, size_t size) override {
    if (data == nullptr || (offset & 3U) != 0 || (size & 3U) != 0 ||
        offset > bytes.size() || size > bytes.size() - offset)
      return FlashOpResult::kFailed;
    const auto* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) bytes[offset + i] &= src[i];
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t page) override {
    if (page >= pages_) return FlashOpResult::kFailed;
    memset(bytes.data() + size_t(page) * 4096U, 0xFF, 4096U);
    return FlashOpResult::kDone;
  }

 private:
  uint32_t pages_;
};

class FixedIncarnation : public ConfigIncarnationSource {
 public:
  bool generate(uint64_t& incarnation) override {
    incarnation = 0x0BADC0FFEE123456ULL;
    return true;
  }
};

class CountingChallenges : public BleChallengeSource {
 public:
  uint8_t next = 0xC0;
  bool fail = false;
  bool generate(uint8_t (&out)[ble_write_auth::kChallengeSize]) override {
    if (fail) return false;
    for (unsigned i = 0; i < sizeof(out); ++i) out[i] = static_cast<uint8_t>(next + i);
    next = static_cast<uint8_t>(next + 0x10);
    return true;
  }
};

// ---- the phone, independently --------------------------------------------
std::vector<uint8_t> writeKey() {
  uint8_t salt[16], root[32];
  for (unsigned i = 0; i < 16; ++i) salt[i] = static_cast<uint8_t>(0x10 + i);
  for (unsigned i = 0; i < 32; ++i) root[i] = static_cast<uint8_t>(0x40 + i);
  std::vector<uint8_t> info(ble_write_auth::kKeyInfoLabel,
                            ble_write_auth::kKeyInfoLabel + 21);
  for (int i = 7; i >= 0; --i) info.push_back(static_cast<uint8_t>(kDevice >> (8 * i)));
  for (int i = 3; i >= 0; --i) info.push_back(static_cast<uint8_t>(kEpoch >> (8 * i)));
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  std::vector<uint8_t> key(32);
  size_t n = key.size();
  assert(ctx && EVP_PKEY_derive_init(ctx) == 1 &&
         EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1 &&
         EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, 16) == 1 &&
         EVP_PKEY_CTX_set1_hkdf_key(ctx, root, 32) == 1 &&
         EVP_PKEY_CTX_add1_hkdf_info(ctx, info.data(), info.size()) == 1 &&
         EVP_PKEY_derive(ctx, key.data(), &n) == 1 && n == 32);
  EVP_PKEY_CTX_free(ctx);
  return key;
}

std::vector<uint8_t> tag(const char* domain, const uint8_t* challenge,
                         const uint8_t* body, size_t body_len) {
  std::vector<uint8_t> input(domain, domain + 21);
  for (int i = 7; i >= 0; --i) input.push_back(static_cast<uint8_t>(kDevice >> (8 * i)));
  for (int i = 3; i >= 0; --i) input.push_back(static_cast<uint8_t>(kEpoch >> (8 * i)));
  input.insert(input.end(), challenge, challenge + 16);
  input.insert(input.end(), body, body + body_len);
  const std::vector<uint8_t> key = writeKey();
  uint8_t mac[32];
  unsigned n = 0;
  assert(HMAC(EVP_sha256(), key.data(), 32, input.data(), input.size(), mac, &n) && n == 32);
  return std::vector<uint8_t>(mac, mac + 16);
}

void putLe32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) { p[i] = static_cast<uint8_t>(v); v >>= 8; }
}
uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

// ---- a Tracker -------------------------------------------------------------
struct Tracker {
  NorFlash sec_flash{8192U, 2U};
  NorFlash cfg_flash{kConfigRegion, storage_config::kFutureConfigRegionPages};
  FixedIncarnation rng;
  SecurityStore security{sec_flash, sec_flash};
  ConfigStore store{cfg_flash, &rng};
  ConfigMutationOwner mutations{store};
  ApplicationRequestService reads{store};
  CountingChallenges challenges;
  BleWriteAuthenticator auth{security, challenges};
  BleWriteService service{auth, mutations, store};
  BleApplicationTransport transport{reads};
  uint32_t now = 1000;
  uint16_t corr = 1;

  explicit Tracker(bool provision = true) {
    assert(store.begin());
    assert(security.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
    if (provision) {
      uint8_t id[16], root[32];
      for (unsigned i = 0; i < 16; ++i) id[i] = static_cast<uint8_t>(0x10 + i);
      for (unsigned i = 0; i < 32; ++i) root[i] = static_cast<uint8_t>(0x40 + i);
      assert(security.commitCredential(id, kEpoch, root));
      for (int i = 0; i < 3000 && security.busy(); ++i) security.poll();
      bool ok = false;
      assert(security.takeCommitResult(ok) && ok);
      assert(security.state() == SecurityState::kProvisioned);
    }
    transport.setWriteGate(&service);
    service.setPolicy(true, StorageRole::kTracker);
    transport.beginSession();
  }

  // One logical request, fragmented like the phone does (12 bytes a frame).
  void send(uint8_t type, const uint8_t* payload, uint16_t length) {
    const uint16_t c = corr++;
    uint16_t at = 0;
    uint8_t index = 0;
    do {
      const uint16_t chunk = static_cast<uint16_t>(length - at > 12 ? 12 : length - at);
      uint8_t frame[20] = {bat::kTransportVersion, type, 0, index,
                           static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8),
                           static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8)};
      frame[2] = static_cast<uint8_t>((index == 0 ? bat::kFlagStart : 0) |
                                      (at + chunk >= length ? bat::kFlagEnd : 0));
      if (chunk) memcpy(frame + 8, payload + at, chunk);
      transport.onFrameReceived(transport.currentSessionGeneration(), frame,
                                static_cast<uint8_t>(8 + chunk), now);
      at = static_cast<uint16_t>(at + chunk);
      ++index;
    } while (at < length);
  }

  // Loop passes until the transport has a whole response; returns it.
  bool answer(uint8_t& type, std::vector<uint8_t>& payload, int passes = 200) {
    for (int i = 0; i < passes && !transport.outboundFramePending(); ++i) {
      store.poll();
      (void)mutations.poll();
      uint32_t id = 0;
      uint8_t out[bat::kWriteResultPayloadSize];
      ConfigMutationResult result;
      if (service.takeAnswer(id, out, result) == BleWriteService::Answer::kReady)
        (void)transport.completeWrite(id, out, sizeof(out));
    }
    if (!transport.outboundFramePending()) return false;
    payload.clear();
    while (transport.outboundFramePending()) {
      uint8_t frame[20];
      uint8_t len = 0;
      assert(transport.peekOutboundFrame(transport.currentSessionGeneration(), frame, len));
      type = frame[1];
      payload.insert(payload.end(), frame + 8, frame + len);
      transport.confirmOutboundFrame(transport.currentSessionGeneration());
    }
    return true;
  }

  std::vector<uint8_t> challenge() {
    send(static_cast<uint8_t>(bat::MessageType::kGetWriteChallengeRequest), nullptr, 0);
    uint8_t type = 0;
    std::vector<uint8_t> p;
    assert(answer(type, p));
    assert(type == static_cast<uint8_t>(bat::MessageType::kGetWriteChallengeResponse));
    assert(p.size() == bat::kWriteChallengePayloadSize && p[0] == 0);
    uint64_t dev = 0;
    for (int i = 0; i < 8; ++i) dev = (dev << 8) | p[1 + i];
    assert(dev == kDevice && le32(p.data() + 9) == kEpoch);
    return std::vector<uint8_t>(p.begin() + 13, p.end());
  }

  std::vector<uint8_t> request(uint8_t op, uint32_t value,
                               const std::vector<uint8_t>& challenge) {
    std::vector<uint8_t> r(bat::kWriteRequestPayloadSize);
    r[0] = op;
    putLe32(r.data() + 1, value);
    const std::vector<uint8_t> t =
        tag(ble_write_auth::kRequestDomain, challenge.data(), r.data(), 5);
    memcpy(r.data() + 5, t.data(), 16);
    return r;
  }

  // Writes and returns the verified result payload (or the error bytes).
  std::vector<uint8_t> write(uint8_t op, uint32_t value, uint8_t& type) {
    const std::vector<uint8_t> c = challenge();
    const std::vector<uint8_t> r = request(op, value, c);
    send(static_cast<uint8_t>(bat::MessageType::kAuthenticatedWriteRequest), r.data(),
         static_cast<uint16_t>(r.size()));
    std::vector<uint8_t> p;
    assert(answer(type, p));
    if (type == static_cast<uint8_t>(bat::MessageType::kAuthenticatedWriteResponse)) {
      assert(p.size() == bat::kWriteResultPayloadSize && p[1] == op);
      const std::vector<uint8_t> t =
          tag(ble_write_auth::kResultDomain, c.data(), p.data(), 14);
      assert(memcmp(t.data(), p.data() + 14, 16) == 0);  // the Tracker said it
    }
    return p;
  }
};

constexpr uint8_t kResult = static_cast<uint8_t>(bat::MessageType::kAuthenticatedWriteResponse);
constexpr uint8_t kError = static_cast<uint8_t>(bat::MessageType::kError);

void testSharedVector() {
  Tracker t;
  assert(writeKey() == unhex(kWriteKey));
  uint64_t dev = 0;
  uint32_t epoch = 0;
  uint8_t c[16];
  assert(t.auth.issue(5, dev, epoch, c) == BleChallengeResult::kIssued);
  assert(dev == kDevice && epoch == kEpoch && c[0] == 0xC0 && c[15] == 0xCF);
  const uint8_t body[5] = {1, 0xB4, 0, 0, 0};  // interval 180
  const std::vector<uint8_t> req = unhex(kRequestTag);
  assert(tag(ble_write_auth::kRequestDomain, c, body, 5) == req);
  assert(t.auth.verify(6, body, 5, req.data()));
  const std::vector<uint8_t> res_body = unhex(kResultBody);
  uint8_t res[16];
  assert(t.auth.signResult(res_body.data(), res_body.size(), res));
  assert(std::vector<uint8_t>(res, res + 16) == unhex(kResultTag));
  // Signed once, then forgotten.
  assert(!t.auth.signResult(res_body.data(), res_body.size(), res));
}

void testChallengeIsSingleUse() {
  Tracker t;
  uint64_t dev = 0;
  uint32_t epoch = 0;
  uint8_t c[16];
  const uint8_t body[5] = {1, 0xB4, 0, 0, 0};
  // Verified once: a second verify with the same tag fails.
  assert(t.auth.issue(5, dev, epoch, c) == BleChallengeResult::kIssued);
  std::vector<uint8_t> good = tag(ble_write_auth::kRequestDomain, c, body, 5);
  assert(t.auth.verify(6, body, 5, good.data()));
  assert(!t.auth.verify(6, body, 5, good.data()));
  // A wrong tag burns the challenge: the right one no longer works.
  assert(t.auth.issue(7, dev, epoch, c) == BleChallengeResult::kIssued);
  good = tag(ble_write_auth::kRequestDomain, c, body, 5);
  std::vector<uint8_t> bad = good;
  bad[3] ^= 1U;
  assert(!t.auth.verify(8, body, 5, bad.data()));
  assert(!t.auth.verify(8, body, 5, good.data()));
  // Expired after 30 s.
  assert(t.auth.issue(100, dev, epoch, c) == BleChallengeResult::kIssued);
  good = tag(ble_write_auth::kRequestDomain, c, body, 5);
  assert(!t.auth.verify(100 + ble_write_auth::kChallengeLifetimeMs, body, 5, good.data()));
  // A tag over different bytes does not fit.
  assert(t.auth.issue(200, dev, epoch, c) == BleChallengeResult::kIssued);
  good = tag(ble_write_auth::kRequestDomain, c, body, 5);
  const uint8_t other[5] = {1, 0xB5, 0, 0, 0};
  assert(!t.auth.verify(201, other, 5, good.data()));
  // Session reset drops it.
  assert(t.auth.issue(300, dev, epoch, c) == BleChallengeResult::kIssued);
  good = tag(ble_write_auth::kRequestDomain, c, body, 5);
  t.auth.reset();
  assert(!t.auth.verify(301, body, 5, good.data()));
  // No entropy: try again later.
  t.challenges.fail = true;
  assert(t.auth.issue(400, dev, epoch, c) == BleChallengeResult::kNoEntropy);
  // An unprovisioned device never issues one.
  Tracker blank(false);
  assert(blank.auth.issue(5, dev, epoch, c) == BleChallengeResult::kNoCredential);
}

void testWritesThroughTheTransport() {
  Tracker t;
  uint8_t type = 0;
  // Interval: applied, durable, answered with the stored state.
  std::vector<uint8_t> p = t.write(bat::kWriteOpInterval, 600, type);
  assert(type == kResult && p[0] == uint8_t(ConfigMutationOutcome::kApplied));
  assert(le32(p.data() + 2) == 600 && t.store.config().tracking_interval_seconds == 600);
  const uint32_t revision = le32(p.data() + 10);
  assert(revision >= 2);
  // Same again: UNCHANGED, still signed.
  p = t.write(bat::kWriteOpInterval, 600, type);
  assert(type == kResult && p[0] == uint8_t(ConfigMutationOutcome::kUnchanged));
  assert(le32(p.data() + 10) == revision);
  // Below the writer floor: INVALID, value kept.
  p = t.write(bat::kWriteOpInterval, 59, type);
  assert(type == kResult && p[0] == uint8_t(ConfigMutationOutcome::kInvalid));
  assert(le32(p.data() + 2) == 600);
  // Bluetooth window: bounds, 0 = off.
  p = t.write(bat::kWriteOpBleWindow, 301, type);
  assert(type == kResult && p[0] == uint8_t(ConfigMutationOutcome::kInvalid));
  p = t.write(bat::kWriteOpBleWindow, 0, type);
  assert(type == kResult && p[0] == uint8_t(ConfigMutationOutcome::kApplied));
  assert(p[6] == 0 && p[7] == 0 && t.store.config().ble_window_seconds == 0);
  // Services: T (tracking) for a Tracker.
  p = t.write(bat::kWriteOpServices,
              config_format::kServiceModeExplicit | (config_format::kServiceTracking << 8), type);
  assert(type == kResult && p[0] == uint8_t(ConfigMutationOutcome::kApplied));
  assert(p[8] == config_format::kServiceModeExplicit && p[9] == config_format::kServiceTracking);
  // The Gateway's receive service is refused on a Tracker (Y2a), signed.
  p = t.write(bat::kWriteOpServices,
              config_format::kServiceModeExplicit |
                  (config_format::kServiceApplicationReceive << 8), type);
  assert(type == kResult && p[0] == uint8_t(ConfigMutationOutcome::kInvalid));
  assert(t.store.config().requested_services == config_format::kServiceTracking);
  // Unknown op and stray high bytes: INVALID, nothing written.
  p = t.write(9, 1, type);
  assert(type == kResult && p[0] == uint8_t(ConfigMutationOutcome::kInvalid));
  p = t.write(bat::kWriteOpServices, 0x00010101U, type);
  assert(type == kResult && p[0] == uint8_t(ConfigMutationOutcome::kInvalid));
  // Reads are untouched by all of this.
  t.send(static_cast<uint8_t>(bat::MessageType::kGetConfigRequest), nullptr, 0);
  assert(t.answer(type, p) && type == 0x81 && le32(p.data() + 2) == 600);
}

void testRefusals() {
  Tracker t;
  uint8_t type = 0;
  std::vector<uint8_t> p;
  // A write without a challenge, and a replay of an accepted one.
  std::vector<uint8_t> c = t.challenge();
  std::vector<uint8_t> r = t.request(bat::kWriteOpInterval, 900, c);
  const uint8_t kWrite = static_cast<uint8_t>(bat::MessageType::kAuthenticatedWriteRequest);
  t.send(kWrite, r.data(), static_cast<uint16_t>(r.size()));
  assert(t.answer(type, p) && type == kResult && p[0] == 0);
  t.send(kWrite, r.data(), static_cast<uint16_t>(r.size()));  // replay
  assert(t.answer(type, p) && type == kError && p[0] == uint8_t(bat::ErrorCode::kAccessDenied));
  // A forged tag (fresh challenge) is denied, and burns that challenge.
  c = t.challenge();
  r = t.request(bat::kWriteOpInterval, 60, c);
  std::vector<uint8_t> forged = r;
  forged[20] ^= 0x80;
  t.send(kWrite, forged.data(), static_cast<uint16_t>(forged.size()));
  assert(t.answer(type, p) && type == kError && p[0] == uint8_t(bat::ErrorCode::kAccessDenied));
  t.send(kWrite, r.data(), static_cast<uint16_t>(r.size()));
  assert(t.answer(type, p) && type == kError && p[0] == uint8_t(bat::ErrorCode::kAccessDenied));
  assert(t.store.config().tracking_interval_seconds == 900);
  // A tag from another challenge, wrong length, too late.
  const std::vector<uint8_t> old = t.challenge();
  c = t.challenge();  // replaces `old`
  r = t.request(bat::kWriteOpInterval, 60, old);
  t.send(kWrite, r.data(), static_cast<uint16_t>(r.size()));
  assert(t.answer(type, p) && type == kError && p[0] == uint8_t(bat::ErrorCode::kAccessDenied));
  c = t.challenge();
  r = t.request(bat::kWriteOpInterval, 60, c);
  t.send(kWrite, r.data(), 20);
  assert(t.answer(type, p) && type == kError && p[0] == uint8_t(bat::ErrorCode::kAccessDenied));
  c = t.challenge();
  r = t.request(bat::kWriteOpInterval, 60, c);
  t.now += ble_write_auth::kChallengeLifetimeMs;
  t.send(kWrite, r.data(), static_cast<uint16_t>(r.size()));
  assert(t.answer(type, p) && type == kError && p[0] == uint8_t(bat::ErrorCode::kAccessDenied));
  assert(t.store.config().tracking_interval_seconds == 900);
  // A reconnect voids the challenge.
  c = t.challenge();
  r = t.request(bat::kWriteOpInterval, 60, c);
  t.transport.endSession(t.transport.currentSessionGeneration());
  t.transport.beginSession();
  t.send(kWrite, r.data(), static_cast<uint16_t>(r.size()));
  assert(t.answer(type, p) && type == kError && p[0] == uint8_t(bat::ErrorCode::kAccessDenied));
  // Busy: a USB change holds the one mutation slot.
  const ConfigMutationRequest usb(ApplicationRequester::kUsb, 77,
                                  defaultApplicationAccess(ApplicationRequester::kUsb),
                                  ConfigMutationKind::kSetTrackingInterval, 1200);
  assert(t.mutations.submit(usb) == ConfigMutationSubmitResult::kAccepted);
  c = t.challenge();
  r = t.request(bat::kWriteOpInterval, 60, c);
  t.send(kWrite, r.data(), static_cast<uint16_t>(r.size()));
  assert(t.answer(type, p, 1) && type == kError && p[0] == uint8_t(bat::ErrorCode::kBusy));
  for (int i = 0; i < 200 && t.mutations.busy(); ++i) {
    t.store.poll();
    (void)t.mutations.poll();
    ConfigMutationResult done;
    (void)t.mutations.takeResult(ApplicationRequester::kUsb, done);
  }
  assert(!t.mutations.busy() && t.store.config().tracking_interval_seconds == 1200);
  // Not writable (a Gateway, the public test credential): no challenge.
  t.service.setPolicy(false, StorageRole::kGateway);
  t.send(static_cast<uint8_t>(bat::MessageType::kGetWriteChallengeRequest), nullptr, 0);
  assert(t.answer(type, p) && type == kError && p[0] == uint8_t(bat::ErrorCode::kAccessDenied));
  // Without a gate (other images) both messages are unsupported.
  ApplicationRequestService reads(t.store);
  BleApplicationTransport plain(reads);
  plain.beginSession();
  const uint8_t frame[8] = {bat::kTransportVersion,
                            static_cast<uint8_t>(bat::MessageType::kGetWriteChallengeRequest),
                            bat::kFlagStart | bat::kFlagEnd, 0, 1, 0, 0, 0};
  plain.onFrameReceived(plain.currentSessionGeneration(), frame, 8, 0);
  uint8_t out[20];
  uint8_t len = 0;
  assert(plain.peekOutboundFrame(plain.currentSessionGeneration(), out, len));
  assert(out[1] == kError && out[8] == uint8_t(bat::ErrorCode::kUnsupported));
  // The authenticated channel is write-only; an open link cannot write.
  ApplicationRequest read(ApplicationRequester::kBle, 5, ApplicationRequestKind::kGetConfig,
                          ApplicationAccessContext(ApplicationAccessChannel::kBleAuthenticated));
  assert(reads.submit(read) != ApplicationSubmitResult::kAccepted);
  const ConfigMutationRequest open(ApplicationRequester::kBle, 6,
                                   defaultApplicationAccess(ApplicationRequester::kBle),
                                   ConfigMutationKind::kSetTrackingInterval, 600);
  assert(t.mutations.submit(open) == ConfigMutationSubmitResult::kAccepted);
  ConfigMutationResult denied;
  assert(t.mutations.takeResult(ApplicationRequester::kBle, denied));
  assert(denied.outcome == ConfigMutationOutcome::kAccessDenied);
}

void testDisconnectWhileWriting() {
  Tracker t;
  const std::vector<uint8_t> c = t.challenge();
  const std::vector<uint8_t> r = t.request(bat::kWriteOpInterval, 300, c);
  t.send(static_cast<uint8_t>(bat::MessageType::kAuthenticatedWriteRequest), r.data(),
         static_cast<uint16_t>(r.size()));
  assert(t.transport.writePending());
  // Stop-and-wait: nothing else is taken while the write is outstanding.
  t.send(static_cast<uint8_t>(bat::MessageType::kGetConfigRequest), nullptr, 0);
  assert(!t.transport.outboundFramePending());
  t.transport.endSession(t.transport.currentSessionGeneration());
  // The change still completes on the Tracker; the answer is dropped and the
  // single mutation slot is freed.
  BleWriteService::Answer last = BleWriteService::Answer::kNone;
  for (int i = 0; i < 200 && last == BleWriteService::Answer::kNone; ++i) {
    t.store.poll();
    (void)t.mutations.poll();
    uint32_t id = 0;
    uint8_t out[bat::kWriteResultPayloadSize];
    ConfigMutationResult result;
    last = t.service.takeAnswer(id, out, result);
  }
  assert(last == BleWriteService::Answer::kDropped);
  assert(!t.mutations.busy() && t.store.config().tracking_interval_seconds == 300);
  // The next session writes normally.
  t.transport.beginSession();
  uint8_t type = 0;
  const std::vector<uint8_t> p = t.write(bat::kWriteOpInterval, 600, type);
  assert(type == kResult && p[0] == uint8_t(ConfigMutationOutcome::kApplied));
}

}  // namespace

int main() {
  testSharedVector();
  testChallengeIsSingleUse();
  testWritesThroughTheTransport();
  testRefusals();
  testDisconnectWhileWriting();
  puts("4c-1b Bluetooth authenticated writes (vector, single-use challenge, "
       "transport, refusals, disconnect): PASS");
  return 0;
}
