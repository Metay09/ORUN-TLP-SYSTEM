// SF5L0 bench round trip, host only.
// Tracker: REAL SecurityStore on fake NOR -> public test credential -> v3 ->
// durable grant for one Gateway, driven only through Sf5l0BenchTestProvisioning.
// Gateway: REAL ProductCustodyStore + GatewayLocalCustodyOwner + signer +
// authorized ingress, keyed by Sf5l0BenchGrantSource.
// Check: the Gateway-signed ACK authenticates in the REAL Tracker verifier.
// Crypto is the independent OpenSSL host port; no RF, no record release.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "custody_ack.h"
#include "custody_ack_verifier.h"
#include "custody_store_product_format.h"
#include "gateway_custody_authorized_ingress.h"
#include "gateway_local_custody_owner.h"
#include "observation_store_control.h"
#include "product_custody_store.h"
#include "security_store.h"
#include "security_v3_format.h"
#include "sf5l0_bench.h"
#include "tlp_v2_product_secure.h"

namespace orun_tlp {
struct CustodyAckVerifierHostPeer {
  static CustodyAckVerifyResult check(
      CustodyAckVerifier& verifier, const uint8_t* ack, size_t length,
      const observation_store_control::ExactObject* cached, uint16_t count) {
    AuthenticatedCustodyAck result{};
    uint16_t selected = UINT16_MAX;
    return verifier.verifyCandidates(ack, length, cached, count, result,
                                     selected);
  }
};
}  // namespace orun_tlp

using namespace orun_tlp;
using Peer = orun_tlp::CustodyAckVerifierHostPeer;
using Step = Sf5l0BenchTestProvisioning::Step;
namespace osc = orun_tlp::observation_store_control;
namespace fmt = orun_tlp::custody_store_product_format;

namespace {
constexpr uint64_t kTracker = UINT64_C(0x0E8ADE7E71531AA3);
constexpr uint64_t kGateway = UINT64_C(0x09A462BD4B275BA5);
constexpr uint64_t kOtherGateway = UINT64_C(0x2300230023002301);

// Security partition: two 4 KiB pages, NOR semantics, synchronous.
struct SecurityFlash : FlashBackend {
  std::array<uint8_t, 8192U> bytes{};
  SecurityFlash() { bytes.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t o, void* out, size_t n) const override {
    if (!out || o > bytes.size() || n > bytes.size() - o) return false;
    memcpy(out, bytes.data() + o, n);
    return true;
  }
  FlashOpResult program(uint32_t o, const void* data, size_t n) override {
    if (!data || !n || (o % 4U) || (n % 4U) || o > bytes.size() ||
        n > bytes.size() - o)
      return FlashOpResult::kFailed;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0U; i < n; ++i)
      if (bytes[o + i] != 0xFFU) return FlashOpResult::kFailed;
    for (size_t i = 0U; i < n; ++i) bytes[o + i] &= src[i];
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t page) override {
    if (page >= 2U) return FlashOpResult::kFailed;
    memset(bytes.data() + page * 4096U, 0xFFU, 4096U);
    return FlashOpResult::kDone;
  }
};

// Gateway product region (two OCQ2 pages), optional async completion.
struct ProductFlash : FlashBackend {
  std::array<uint8_t, fmt::kPageSize * 2U> bytes{};
  bool async = false, pending = false;
  ProductFlash() { bytes.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t o, void* dst, size_t n) const override {
    if (!dst || o > bytes.size() || n > bytes.size() - o) return false;
    memcpy(dst, bytes.data() + o, n);
    return true;
  }
  FlashOpResult program(uint32_t o, const void* src, size_t n) override {
    if (pending || !src || !n || n > 144 || o % 4 || n % 4 ||
        o > bytes.size() || n > bytes.size() - o)
      return FlashOpResult::kFailed;
    const uint8_t* p = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; i++)
      if (bytes[o + i] != 0xFFU) return FlashOpResult::kFailed;
    for (size_t i = 0; i < n; i++) bytes[o + i] &= p[i];
    if (async) { pending = true; return FlashOpResult::kPending; }
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t) override { return FlashOpResult::kFailed; }
  FlashOpResult pollPending() override {
    if (!pending) return FlashOpResult::kFailed;
    pending = false;
    return FlashOpResult::kDone;
  }
};

void drive(SecurityStore& s, Sf5l0BenchTestProvisioning& p) {
  for (unsigned pass = 0U; pass < 20000U; ++pass) {
    s.poll();
    p.poll();
    if (p.step() == Step::kDone || p.step() == Step::kFailed) break;
  }
}

void makeFrame(osc::ExactObject& object, uint64_t tracker, uint32_t epoch,
               uint64_t counter) {
  tlp::ProductSecureFrame f{};
  f.app_family = tlp::kProductAppFamilyPeriodic;
  f.path_flags = static_cast<uint8_t>(tlp::kProductPathFlagRelayAllowed |
                                      tlp::kProductPathFlagCustodyRequested);
  f.security_profile = tlp::kProductSecurityProfileRootAeadV1;
  f.ciphertext_len = tlp::kPeriodicPlaintextSize;
  f.device_id = tracker;
  f.key_epoch = epoch;
  f.security_counter = counter;
  for (size_t i = 0; i < tlp::kPeriodicPlaintextSize; ++i)
    f.ciphertext[i] = static_cast<uint8_t>(i * 3U + 1U);
  for (size_t i = 0; i < tlp::kProductSecureTagSize; ++i)
    f.tag[i] = static_cast<uint8_t>(0x50U + i);
  object.identity.incarnation = 77U;
  object.identity.sequence = 5U;
  object.record_kind = observation_store_format::RecordKind::kPeriodic;
  object.object_size = tlp::kPeriodicFrameSize;
  assert(tlp::encodeProductSecureFrame(f, object.object,
                                       sizeof(object.object)) ==
         tlp::ProductSecureCodecResult::kOk);
}

// Gateway side: receive one exact frame, hold it durably, return its ACK.
bool gatewayAck(uint64_t gateway_id, const osc::ExactObject& object,
                bool async, uint8_t (&ack)[custody_ack::kWireSize]) {
  ProductFlash flash;
  flash.async = async;
  ProductCustodyStore store(flash, 2U);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  Sf5l0BenchGrantSource grants(gateway_id);
  assert(grants.verifiedForLocalGateway(gateway_id));
  assert(!grants.verifiedForLocalGateway(gateway_id ^ 1U));
  GatewayCustodyAuthorizedIngress ingress(owner, grants, gateway_id);
  const auto started = ingress.receive(object.object, object.object_size);
  if (started == GatewayCustodyAuthorizedIngress::Result::kDenied)
    return false;
  assert(started == GatewayCustodyAuthorizedIngress::Result::kStarted);
  for (unsigned i = 0U; i < 200U && ingress.pending(); ++i) {
    ingress.service();
    // takeAuthorizedAck consumes the pending state; call once ready.
    if (owner.ackReady()) break;
  }
  return ingress.takeAuthorizedAck(ack);
}

void testTrackerProvisioningFromBlank() {
  SecurityFlash flash;
  SecurityStore s(flash, flash);
  assert(s.begin(DeviceIdentity::fromLegacyUint64(kTracker)));
  assert(s.state() == SecurityState::kUnprovisioned);
  Sf5l0BenchTestProvisioning p(s);
  assert(!p.start(0U) && p.step() == Step::kFailed);
  assert(p.start(kGateway));
  drive(s, p);
  assert(p.step() == Step::kDone);
  assert(Sf5l0BenchTestProvisioning::holdsTestCredential(s));
  assert(Sf5l0BenchTestProvisioning::activePageVersion(s) ==
         security_v3_format::kVersion);
  uint32_t generation = 0U, floor = 99U;
  assert(Sf5l0BenchTestProvisioning::grantFor(s, kGateway, generation, floor));
  assert(generation == sf5l0_bench::kTestGrantGeneration);
  assert(floor == sf5l0_bench::kTestCustodyFloor);
  assert(!Sf5l0BenchTestProvisioning::grantFor(s, kOtherGateway, generation,
                                               floor));
  assert(Sf5l0BenchTestProvisioning::firstGrantedGateway(s) == kGateway);

  // Re-running for the same Gateway is idempotent (no new journal entry).
  assert(p.start(kGateway));
  drive(s, p);
  assert(p.step() == Step::kDone);
  assert(Sf5l0BenchTestProvisioning::grantFor(s, kGateway, generation, floor) &&
         generation == 1U);

  // Survives reboot: recovered from flash, not RAM.
  SecurityStore rebooted(flash, flash);
  assert(rebooted.begin(DeviceIdentity::fromLegacyUint64(kTracker)));
  // Boot may start a TX-counter reservation; the verifier fails closed until
  // the store is idle (the device loop polls it the same way).
  for (unsigned i = 0U; i < 3000U && rebooted.busy(); ++i) rebooted.poll();
  assert(!rebooted.busy());
  assert(Sf5l0BenchTestProvisioning::holdsTestCredential(rebooted));
  assert(Sf5l0BenchTestProvisioning::grantFor(rebooted, kGateway, generation,
                                              floor) &&
         generation == 1U && floor == 0U);

  // End to end: Gateway-signed ACK authenticates in the real verifier.
  osc::ExactObject object{};
  makeFrame(object, kTracker, sf5l0_bench::kTestKeyEpoch, 42U);
  for (int async = 0; async <= 1; ++async) {
    uint8_t ack[custody_ack::kWireSize]{};
    assert(gatewayAck(kGateway, object, async != 0, ack));
    custody_ack::Header header{};
    assert(custody_ack::decodeHeader(ack, sizeof(ack), header));
    assert(header.relay_allowed && header.tracker_device_id == kTracker &&
           header.gateway_device_id == kGateway &&
           header.key_epoch == sf5l0_bench::kTestKeyEpoch &&
           header.custody_floor == 0U && header.grant_generation == 1U);
    CustodyAckVerifier verifier(rebooted);
    assert(Peer::check(verifier, ack, sizeof(ack), &object, 1U) ==
           CustodyAckVerifyResult::kAuthenticated);

    // Tampered object or ACK never authenticates.
    osc::ExactObject other = object;
    other.object[60] ^= 1U;
    assert(Peer::check(verifier, ack, sizeof(ack), &other, 1U) !=
           CustodyAckVerifyResult::kAuthenticated);
    for (size_t i = 0U; i < sizeof(ack); ++i) {
      uint8_t bad[custody_ack::kWireSize]{};
      memcpy(bad, ack, sizeof(bad));
      bad[i] ^= 1U;
      assert(Peer::check(verifier, bad, sizeof(bad), &object, 1U) !=
             CustodyAckVerifyResult::kAuthenticated);
    }
  }

  // A Gateway the Tracker never granted signs an ACK the Tracker refuses.
  uint8_t stranger[custody_ack::kWireSize]{};
  assert(gatewayAck(kOtherGateway, object, false, stranger));
  CustodyAckVerifier verifier(rebooted);
  assert(Peer::check(verifier, stranger, sizeof(stranger), &object, 1U) ==
         CustodyAckVerifyResult::kNoDurableGrant);

  // Wrong key epoch: the Gateway test source refuses to grant at all.
  osc::ExactObject wrong_epoch{};
  makeFrame(wrong_epoch, kTracker, sf5l0_bench::kTestKeyEpoch + 1U, 43U);
  uint8_t none[custody_ack::kWireSize]{};
  assert(!gatewayAck(kGateway, wrong_epoch, false, none));
}

void testForeignCredentialNeverTouched() {
  SecurityFlash flash;
  SecurityStore s(flash, flash);
  assert(s.begin(DeviceIdentity::fromLegacyUint64(kTracker)));
  uint8_t id[security_format::kCredentialIdSize]{};
  uint8_t root[security_format::kKRootSize]{};
  for (unsigned i = 0U; i < sizeof(id); ++i) id[i] = static_cast<uint8_t>(i + 3U);
  for (unsigned i = 0U; i < sizeof(root); ++i)
    root[i] = static_cast<uint8_t>(0x90U + i);
  assert(s.commitCredential(id, 7U, root));
  for (unsigned i = 0U; i < 3000U && s.busy(); ++i) s.poll();
  bool ok = false;
  assert(s.takeCommitResult(ok) && ok);
  const std::array<uint8_t, 8192U> before = flash.bytes;

  Sf5l0BenchTestProvisioning p(s);
  assert(!p.start(kGateway));
  assert(p.step() == Step::kFailed);
  drive(s, p);
  assert(flash.bytes == before);
  assert(!Sf5l0BenchTestProvisioning::holdsTestCredential(s));
  assert(Sf5l0BenchTestProvisioning::firstGrantedGateway(s) == 0U);
  assert(Sf5l0BenchTestProvisioning::activePageVersion(s) ==
         security_format::kVersionV2);
}

}  // namespace

int main() {
  testTrackerProvisioningFromBlank();
  testForeignCredentialNeverTouched();
  puts("SF5L0 bench test-key provisioning + Gateway ACK -> Tracker verifier: "
       "PASS (HOST ONLY)");
  return 0;
}
