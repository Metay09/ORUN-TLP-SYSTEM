// Y2a: the Tracker/Gateway runtime built the way the device firmware builds
// it, i.e. WITHOUT ORUN_SF5L0_BENCH_TEST_KEYS. Host only (OpenSSL crypto port,
// fake flash). This configuration leaves out the Gateway-only Edge accept code
// (the only remaining user of the test root, until Y2b); the runner checks its
// object file holds no test root.
//
// Pins:
//  - a blank Tracker still gets a REAL credential over USB and its Gateway
//    grant (BENCH PROVISION path);
//  - a Tracker that still holds the old test credential is recognised from
//    its public id + epoch alone (test_credential=yes), without any root;
//  - the Gateway grant source never falls back to the public test key: a
//    test-epoch frame gets no custody and no ACK, a real one only with a
//    backend grant, and that ACK is authenticated by the real verifier;
//  - without the test root the Edge accept MAC is refused.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>

#ifdef ORUN_SF5L0_BENCH_TEST_KEYS
#error "this test must be built like the product images (no test keys)"
#endif

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

#ifdef ORUN_SF5L0_HAS_TEST_ROOT
#error "this configuration must not compile the public test root"
#endif

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

// The backend vector of test_step3a_credentials.cpp (credential 10..1F,
// root 40..5F, epoch 1; Gateway grant floor 0, generation 1).
const char kGrantLine[] =
    "BENCH GRANT 0E8ADE7E71531AA3 00000001 00000000 00000001 "
    "91AE7D108998E87565977EA606FE6A13F6493DEAAF5C46C791BB02B6594A0D60 4BE6F488";

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

struct ProductFlash : FlashBackend {
  std::array<uint8_t, fmt::kPageSize * 2U> bytes{};
  ProductFlash() { bytes.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t o, void* dst, size_t n) const override {
    if (!dst || o > bytes.size() || n > bytes.size() - o) return false;
    memcpy(dst, bytes.data() + o, n);
    return true;
  }
  FlashOpResult program(uint32_t o, const void* src, size_t n) override {
    if (!src || !n || n > 144 || o % 4 || n % 4 || o > bytes.size() ||
        n > bytes.size() - o)
      return FlashOpResult::kFailed;
    const uint8_t* p = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; i++)
      if (bytes[o + i] != 0xFFU) return FlashOpResult::kFailed;
    for (size_t i = 0; i < n; i++) bytes[o + i] &= p[i];
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t) override { return FlashOpResult::kFailed; }
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

bool ackWith(Sf5l0BenchGrantSource& grants, const osc::ExactObject& object,
             uint8_t (&ack)[custody_ack::kWireSize]) {
  ProductFlash flash;
  ProductCustodyStore store(flash, 2U);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  GatewayCustodyAuthorizedIngress ingress(owner, grants, kGateway);
  if (ingress.receive(object.object, object.object_size) !=
      GatewayCustodyAuthorizedIngress::Result::kStarted)
    return false;
  for (unsigned i = 0U; i < 200U && ingress.pending() && !owner.ackReady(); ++i)
    ingress.service();
  return ingress.takeAuthorizedAck(ack);
}

void testRealCredentialStillProvisions() {
  uint8_t id[security_format::kCredentialIdSize]{};
  uint8_t root[security_format::kKRootSize]{};
  for (unsigned i = 0U; i < sizeof(id); ++i) id[i] = static_cast<uint8_t>(0x10U + i);
  for (unsigned i = 0U; i < sizeof(root); ++i) root[i] = static_cast<uint8_t>(0x40U + i);
  SecurityFlash flash;
  SecurityStore s(flash, flash);
  assert(s.begin(DeviceIdentity::fromLegacyUint64(kTracker)));
  Sf5l0BenchTestProvisioning p(s);
  assert(p.startWithCredential(kGateway, id, 1U, root));
  drive(s, p);
  assert(p.step() == Step::kDone);
  assert(!Sf5l0BenchTestProvisioning::holdsTestCredential(s));
  assert(Sf5l0BenchTestProvisioning::firstGrantedGateway(s) == kGateway);

  // Gateway: the real frame gets custody only with the backend grant, and
  // the real Tracker verifier authenticates that ACK.
  osc::ExactObject object{};
  makeFrame(object, kTracker, 1U, 42U);
  uint8_t ack[custody_ack::kWireSize]{};
  Sf5l0BenchGrantSource bare(kGateway);
  assert(!ackWith(bare, object, ack));
  uint64_t tracker = 0U;
  uint32_t epoch = 0U, gfloor = 0U, ggen = 0U;
  uint8_t key[32]{};
  assert(sf5l0ParseGrantLine(kGrantLine, strlen(kGrantLine), tracker, epoch,
                             gfloor, ggen, key) == Sf5l0LineResult::kOk);
  Sf5l0BenchGrantSource grants(kGateway);
  assert(grants.addGrant(tracker, epoch, gfloor, ggen, key) ==
         Sf5l0BenchGrantSource::AddResult::kAdded);
  assert(ackWith(grants, object, ack));
  for (unsigned i = 0U; i < 3000U && s.busy(); ++i) s.poll();
  CustodyAckVerifier verifier(s);
  assert(Peer::check(verifier, ack, sizeof(ack), &object, 1U) ==
         CustodyAckVerifyResult::kAuthenticated);
}

void testTestKeyTrackerIsRefused() {
  // A collar provisioned earlier with the old test credential: same public id
  // and epoch. The product image never knew its root; any bytes stand in.
  uint8_t root[security_format::kKRootSize]{};
  for (unsigned i = 0U; i < sizeof(root); ++i) root[i] = static_cast<uint8_t>(0xC0U + i);
  SecurityFlash flash;
  SecurityStore s(flash, flash);
  assert(s.begin(DeviceIdentity::fromLegacyUint64(kTracker)));
  assert(s.commitCredential(sf5l0_bench::kTestCredentialId,
                            sf5l0_bench::kTestKeyEpoch, root));
  for (unsigned i = 0U; i < 3000U && s.busy(); ++i) s.poll();
  bool ok = false;
  assert(s.takeCommitResult(ok) && ok);
  assert(Sf5l0BenchTestProvisioning::holdsTestCredential(s));   // reported

  // Gateway: no grant source hands out a key for the test epoch any more,
  // even with unrelated RAM grants present.
  osc::ExactObject old{};
  makeFrame(old, kTracker, sf5l0_bench::kTestKeyEpoch, 7U);
  uint8_t ack[custody_ack::kWireSize]{};
  Sf5l0BenchGrantSource bare(kGateway);
  assert(!ackWith(bare, old, ack));
  assert(bare.grantsRefused() >= 1U && bare.grantsIssued() == 0U);
  uint8_t key[32]{};
  for (unsigned i = 0U; i < sizeof(key); ++i) key[i] = static_cast<uint8_t>(i);
  Sf5l0BenchGrantSource some(kGateway);
  assert(some.addGrant(kTracker, 1U, 0U, 1U, key) ==
         Sf5l0BenchGrantSource::AddResult::kAdded);
  assert(!ackWith(some, old, ack));
}

void testNoEdgeMacWithoutRoot() {
  uint8_t object[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t out[32]{};
  assert(!Sf5l0EdgeHandoff::mac(kGateway, 1U, 2U, 3U, object, sizeof(object), out));
}

}  // namespace

int main() {
  testRealCredentialStillProvisions();
  testTestKeyTrackerIsRefused();
  testNoEdgeMacWithoutRoot();
  puts("Y2a product keys: real credential provisions, test-key Tracker is "
       "reported and refused custody, no test root: PASS (HOST ONLY)");
  return 0;
}
