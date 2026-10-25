// Step 3A: real per-Tracker credential over USB + backend-derived scoped
// custody ACK key on the Gateway. Host only (OpenSSL crypto port, fake flash).
//
// Pins:
//  - BENCH PROVISION / BENCH GRANT line parsers (exact length, hex, CRC typo
//    guard) against vectors produced by backend/orun_credentials.py;
//  - a blank Tracker commits a REAL credential, migrates to v3 and grants the
//    Gateway; repeating it is idempotent; any other credential (incl. the
//    public test one) is never overwritten;
//  - a Gateway with the backend-derived RAM grant ACKs that Tracker's frames
//    and the REAL Tracker verifier authenticates the ACK; without the grant,
//    with a wrong key, or for another epoch, nothing authenticates;
//  - the test-key path keeps working for old test-epoch frames.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <string>

#define main sf5l0_round_trip_host_main
#include "test_sf5l0_bench_round_trip.cpp"
#undef main

namespace {

// backend/orun_credentials.py with credential_id = 10..1F, root = 40..5F,
// epoch 1, Tracker kTracker, Gateway kGateway, floor 0, generation 1.
const char kProvisionLine[] =
    "BENCH PROVISION 101112131415161718191A1B1C1D1E1F 00000001 "
    "404142434445464748494A4B4C4D4E4F505152535455565758595A5B5C5D5E5F "
    "09A462BD4B275BA5 814DBE0E";
const char kGrantLine[] =
    "BENCH GRANT 0E8ADE7E71531AA3 00000001 00000000 00000001 "
    "91AE7D108998E87565977EA606FE6A13F6493DEAAF5C46C791BB02B6594A0D60 4BE6F488";

void testParsers() {
  uint8_t id[security_format::kCredentialIdSize]{};
  uint8_t root[security_format::kKRootSize]{};
  uint32_t epoch = 0U;
  uint64_t gw = 0U;
  assert(sf5l0ParseProvisionLine(kProvisionLine, strlen(kProvisionLine), id,
                                 epoch, root, gw) == Sf5l0LineResult::kOk);
  assert(epoch == 1U && gw == kGateway && id[0] == 0x10U && id[15] == 0x1FU &&
         root[0] == 0x40U && root[31] == 0x5FU);
  std::string bad = kProvisionLine;
  bad[20] = bad[20] == '1' ? '2' : '1';                     // typo in the id
  assert(sf5l0ParseProvisionLine(bad.data(), bad.size(), id, epoch, root, gw) ==
         Sf5l0LineResult::kBadCrc);
  bad = kProvisionLine;
  bad[20] = 'X';
  assert(sf5l0ParseProvisionLine(bad.data(), bad.size(), id, epoch, root, gw) ==
         Sf5l0LineResult::kMalformed);
  assert(sf5l0ParseProvisionLine(kProvisionLine, strlen(kProvisionLine) - 1U, id,
                                 epoch, root, gw) == Sf5l0LineResult::kMalformed);
  bad = kProvisionLine;
  bad[15] = '_';
  assert(sf5l0ParseProvisionLine(bad.data(), bad.size(), id, epoch, root, gw) ==
         Sf5l0LineResult::kMalformed);

  uint64_t tracker = 0U;
  uint32_t floor = 9U, generation = 0U;
  uint8_t key[32]{};
  assert(sf5l0ParseGrantLine(kGrantLine, strlen(kGrantLine), tracker, epoch, floor,
                             generation, key) == Sf5l0LineResult::kOk);
  assert(tracker == kTracker && epoch == 1U && floor == 0U && generation == 1U &&
         key[0] == 0x91U && key[31] == 0x60U);
  bad = kGrantLine;
  bad[bad.size() - 1] = bad[bad.size() - 1] == '8' ? '9' : '8';
  assert(sf5l0ParseGrantLine(bad.data(), bad.size(), tracker, epoch, floor,
                             generation, key) == Sf5l0LineResult::kBadCrc);
  assert(sf5l0ParseGrantLine(kProvisionLine, strlen(kProvisionLine), tracker, epoch,
                             floor, generation, key) == Sf5l0LineResult::kMalformed);
}

struct RealCredential {
  uint8_t id[security_format::kCredentialIdSize]{};
  uint8_t root[security_format::kKRootSize]{};
  uint32_t epoch = 1U;
  RealCredential() {
    for (unsigned i = 0U; i < sizeof(id); ++i) id[i] = static_cast<uint8_t>(0x10U + i);
    for (unsigned i = 0U; i < sizeof(root); ++i) root[i] = static_cast<uint8_t>(0x40U + i);
  }
};

// Gateway: receive one exact frame with this grant source, return its ACK.
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

void testRealCredentialEndToEnd() {
  const RealCredential c;
  SecurityFlash flash;
  SecurityStore s(flash, flash);
  assert(s.begin(DeviceIdentity::fromLegacyUint64(kTracker)));
  Sf5l0BenchTestProvisioning p(s);
  assert(!p.startWithCredential(kGateway, c.id, 0U, c.root));       // epoch 0
  assert(p.startWithCredential(kGateway, c.id, c.epoch, c.root));
  drive(s, p);
  assert(p.step() == Step::kDone);
  assert(Sf5l0BenchTestProvisioning::holdsCredential(s, c.id, c.epoch, c.root));
  assert(!Sf5l0BenchTestProvisioning::holdsTestCredential(s));
  assert(Sf5l0BenchTestProvisioning::firstGrantedGateway(s) == kGateway);
  uint32_t generation = 0U, floor = 9U;
  assert(Sf5l0BenchTestProvisioning::grantFor(s, kGateway, generation, floor));
  assert(generation == 1U && floor == 0U);   // what the backend vector assumes
  uint8_t cred_out[security_format::kCredentialIdSize]{};
  uint32_t epoch_out = 0U;
  assert(Sf5l0BenchTestProvisioning::credentialInfo(s, cred_out, epoch_out) &&
         epoch_out == c.epoch && memcmp(cred_out, c.id, sizeof(cred_out)) == 0);

  // Idempotent with the same credential; refused for any other one.
  const std::array<uint8_t, 8192U> before = flash.bytes;
  assert(p.startWithCredential(kGateway, c.id, c.epoch, c.root));
  drive(s, p);
  assert(p.step() == Step::kDone && flash.bytes == before);
  RealCredential other;
  other.root[5] ^= 1U;
  assert(!p.startWithCredential(kGateway, other.id, other.epoch, other.root));
  assert(!p.start(kGateway));                                         // test key
  drive(s, p);
  assert(flash.bytes == before);

  // Gateway: no grant for this epoch -> no custody, no ACK.
  osc::ExactObject object{};
  makeFrame(object, kTracker, c.epoch, 42U);
  uint8_t ack[custody_ack::kWireSize]{};
  Sf5l0BenchGrantSource bare(kGateway);
  assert(!ackWith(bare, object, ack));

  // Backend-derived RAM grant (exact backend line) -> ACK the REAL verifier
  // authenticates.
  uint64_t tracker = 0U;
  uint32_t epoch = 0U, gfloor = 0U, ggen = 0U;
  uint8_t key[32]{};
  assert(sf5l0ParseGrantLine(kGrantLine, strlen(kGrantLine), tracker, epoch, gfloor,
                             ggen, key) == Sf5l0LineResult::kOk);
  Sf5l0BenchGrantSource grants(kGateway);
  assert(grants.addGrant(tracker, epoch, gfloor, ggen, key) ==
         Sf5l0BenchGrantSource::AddResult::kAdded);
  assert(grants.addGrant(tracker, epoch, gfloor, ggen, key) ==
         Sf5l0BenchGrantSource::AddResult::kReplaced);
  assert(grants.ramGrants() == 1U);
  assert(ackWith(grants, object, ack));
  for (unsigned i = 0U; i < 3000U && s.busy(); ++i) s.poll();
  CustodyAckVerifier verifier(s);
  assert(Peer::check(verifier, ack, sizeof(ack), &object, 1U) ==
         CustodyAckVerifyResult::kAuthenticated);

  // A wrong key never authenticates.
  uint8_t wrong[32]{};
  memcpy(wrong, key, sizeof(wrong));
  wrong[0] ^= 1U;
  Sf5l0BenchGrantSource bad(kGateway);
  assert(bad.addGrant(tracker, epoch, gfloor, ggen, wrong) ==
         Sf5l0BenchGrantSource::AddResult::kAdded);
  uint8_t bad_ack[custody_ack::kWireSize]{};
  assert(ackWith(bad, object, bad_ack));
  assert(Peer::check(verifier, bad_ack, sizeof(bad_ack), &object, 1U) !=
         CustodyAckVerifyResult::kAuthenticated);

  // The test-key path is unchanged for old test-epoch frames (but this
  // Tracker, now on a real credential, does not accept test-key ACKs).
  osc::ExactObject old{};
  makeFrame(old, kTracker, sf5l0_bench::kTestKeyEpoch, 7U);
  uint8_t old_ack[custody_ack::kWireSize]{};
  assert(ackWith(grants, old, old_ack));
  assert(Peer::check(verifier, old_ack, sizeof(old_ack), &old, 1U) !=
         CustodyAckVerifyResult::kAuthenticated);

  // Table limits.
  Sf5l0BenchGrantSource full(kGateway);
  for (uint64_t t = 1U; t <= Sf5l0BenchGrantSource::kRamGrants; ++t)
    assert(full.addGrant(t, 1U, 0U, 1U, key) == Sf5l0BenchGrantSource::AddResult::kAdded);
  assert(full.addGrant(99U, 1U, 0U, 1U, key) == Sf5l0BenchGrantSource::AddResult::kFull);
  assert(full.addGrant(0U, 1U, 0U, 1U, key) == Sf5l0BenchGrantSource::AddResult::kInvalid);
  assert(full.addGrant(5U, 0U, 0U, 1U, key) == Sf5l0BenchGrantSource::AddResult::kInvalid);
}

}  // namespace

int main() {
  sf5l0_round_trip_host_main();   // the bench test-key path still passes
  testParsers();
  testRealCredentialEndToEnd();
  puts("Step 3A real Tracker credential over USB + backend-derived Gateway "
       "grant -> real Tracker verifier, parsers, refusal of foreign "
       "credentials: PASS (HOST ONLY)");
  return 0;
}
