// SF5K3Y: Gateway-only security-area grant snapshot recovery.
// The verifier is deliberately a TEST FAKE: this test does not qualify
// cryptographic signatures, issuer provisioning or tamper resistance.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "gateway_grant_snapshot_reader.h"
#include "observation_store_format.h"

using namespace orun_tlp;
namespace fmt = orun_tlp::gateway_grant_format;
namespace {
constexpr uint64_t kGateway = UINT64_C(0x2300230023002301);
constexpr uint64_t kTracker = UINT64_C(0x0102030405060708);

void be32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value >> 24U);
  out[1] = static_cast<uint8_t>(value >> 16U);
  out[2] = static_cast<uint8_t>(value >> 8U);
  out[3] = static_cast<uint8_t>(value);
}
void be64(uint8_t* out, uint64_t value) {
  for (unsigned i = 0U; i < 8U; ++i)
    out[i] = static_cast<uint8_t>(value >> (56U - i * 8U));
}
void fakeProof(const uint8_t* data, size_t size, uint8_t (&out)[64]) {
  const uint32_t crc = observation_store_format::crc32(data, size);
  for (size_t i = 0U; i < sizeof(out); ++i)
    out[i] = static_cast<uint8_t>(
        (crc >> ((i % 4U) * 8U)) ^ (i * 23U + 0x5BU));
}
struct FakeVerifier final : GatewayEnrollmentProofVerifier {
  bool trust = true;
  mutable unsigned checked = 0U;
  bool verify(const uint8_t* payload, size_t size,
              const uint8_t (&proof)[fmt::kProofBytes]) const override {
    ++checked;
    if (!trust || payload == nullptr || size != fmt::kPayloadBytes)
      return false;
    uint8_t expected[fmt::kProofBytes]{};
    fakeProof(payload, size, expected);
    return memcmp(expected, proof, sizeof(expected)) == 0;
  }
};
struct Nor final : FlashBackend {
  std::array<uint8_t, fmt::kPageBytes * fmt::kPageCount> bytes{};
  bool begin_ok = true, read_ok = true, unreconciled = false;
  unsigned erases = 0U, writes = 0U;
  Nor() { bytes.fill(0xFFU); }
  bool begin() override { return begin_ok; }
  bool read(uint32_t offset, void* out, size_t count) const override {
    if (!read_ok || out == nullptr || offset > bytes.size() ||
        count > bytes.size() - offset) return false;
    memcpy(out, bytes.data() + offset, count);
    return true;
  }
  bool hasUnreconciledMutation() const override { return unreconciled; }
  FlashOpResult program(uint32_t, const void*, size_t) override {
    ++writes;
    return FlashOpResult::kFailed;
  }
  FlashOpResult erasePage(uint32_t) override {
    ++erases;
    return FlashOpResult::kFailed;
  }
};
void placeGrant(uint8_t* data, size_t slot, uint64_t tracker, uint32_t epoch,
                uint32_t floor, uint32_t generation, uint8_t key_seed) {
  assert(slot < fmt::kGrantSlots);
  uint8_t* s = data + fmt::kHeaderBytes + slot * fmt::kGrantBytes;
  be64(s, tracker);
  be32(s + 8U, epoch);
  be32(s + 12U, floor);
  be32(s + 16U, generation);
  be32(s + 20U, 1U);
  for (unsigned i = 0U; i < 32U; ++i)
    s[24U + i] = static_cast<uint8_t>(key_seed + i);
}
void makePage(Nor& nor, uint16_t page, uint64_t id, uint64_t generation,
              uint64_t tracker = kTracker, uint32_t grant_generation = 3U,
              uint8_t key_seed = 17U) {
  assert(page < fmt::kPageCount);
  auto* raw = nor.bytes.data() + page * fmt::kPageBytes;
  memset(raw, 0xFF, fmt::kPageBytes);
  memset(raw, 0, fmt::kStoredBytes);
  be32(raw, fmt::kMagic);
  raw[4] = fmt::kVersion;
  raw[5] = static_cast<uint8_t>(fmt::kGrantSlots);
  be64(raw + 8U, id);
  be64(raw + 16U, generation);
  be32(raw + 24U, 1U);
  placeGrant(raw, 0U, tracker, 7U, 10U, grant_generation, key_seed);

  uint8_t proof[fmt::kProofBytes]{};
  fakeProof(raw, fmt::kPayloadBytes, proof);
  memcpy(raw + fmt::kPayloadBytes, proof, sizeof(proof));
  be32(raw + fmt::kCrcOffset,
       observation_store_format::crc32(raw, fmt::kCrcOffset));
  be32(raw + fmt::kCommitOffset, 0U);  // Simulated committed snapshot.
}
void testEmptyAndRejected() {
  for (unsigned scenario = 0U; scenario < 13U; ++scenario) {
    Nor nor;
    FakeVerifier verifier;
    uint64_t id = kGateway;
    bool owner = true, tracker_security_mounted = false;
    GatewayGrantSnapshotReader::Status expected =
        GatewayGrantSnapshotReader::Status::kRejected;
    switch (scenario) {
      case 0U:
        expected = GatewayGrantSnapshotReader::Status::kEmpty;
        break;
      case 1U:
        owner = false;
        expected = GatewayGrantSnapshotReader::Status::kWrongOwner;
        break;
      case 2U:
        tracker_security_mounted = true;
        expected = GatewayGrantSnapshotReader::Status::kWrongOwner;
        break;
      case 3U:
        id = 0U;
        expected = GatewayGrantSnapshotReader::Status::kWrongOwner;
        break;
      case 4U:
        nor.begin_ok = false;
        expected = GatewayGrantSnapshotReader::Status::kUnavailable;
        break;
      case 5U:
        nor.unreconciled = true;
        expected = GatewayGrantSnapshotReader::Status::kUnavailable;
        break;
      case 6U:
        memcpy(nor.bytes.data(), "ORS1", 4U);
        break;
      case 7U:
        memcpy(nor.bytes.data(), "OGK1", 4U); // torn/uncommitted
        break;
      case 8U:
        makePage(nor, 0U, kGateway, 1U);
        nor.bytes[60] ^= 0x04U; // CRC/proof mismatch
        break;
      case 9U:
        makePage(nor, 0U, kGateway, 1U);
        verifier.trust = false; // valid CRC, unauthorized signature
        break;
      case 10U:
        makePage(nor, 0U, kGateway + 1U, 1U);
        break;
      case 11U:
        makePage(nor, 0U, kGateway, 1U);
        makePage(nor, 1U, kGateway, 1U); // ambiguous equal generations
        break;
      case 12U:
        makePage(nor, 0U, kGateway, 1U);
        nor.bytes[fmt::kPageBytes - 1U] = 0U; // dirty page tail
        break;
    }
    GatewayGrantSnapshotReader reader(nor, verifier);
    assert(reader.begin(id, owner, tracker_security_mounted) == expected);
    assert(reader.begin(kGateway, true, false) == expected);
    assert(!reader.verifiedForLocalGateway(kGateway));
    GatewayCustodyAckScope scope{};
    uint8_t key[32]{};
    assert(!reader.activeGrant(kTracker, 7U, scope, key));
    assert(nor.erases == 0U && nor.writes == 0U);
  }
}
void testCommittedRebootAndSelection() {
  Nor nor;
  FakeVerifier verifier;
  makePage(nor, 0U, kGateway, 1U, kTracker, 2U, 5U);
  makePage(nor, 1U, kGateway, 2U, kTracker, 4U, 23U);
  GatewayGrantSnapshotReader reader(nor, verifier);
  assert(reader.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kReady);
  assert(verifier.checked == 3U);  // Final selected snapshot is verified again.
  assert(reader.verifiedForLocalGateway(kGateway));
  assert(!reader.verifiedForLocalGateway(kGateway + 1U));
  GatewayCustodyAckScope scope{};
  uint8_t key[32]{};
  assert(reader.activeGrant(kTracker, 7U, scope, key));
  assert(scope.tracker_device_id == kTracker);
  assert(scope.gateway_device_id == kGateway);
  assert(scope.key_epoch == 7U);
  assert(scope.custody_floor == 10U);
  assert(scope.grant_generation == 4U);
  for (unsigned i = 0U; i < sizeof(key); ++i)
    assert(key[i] == static_cast<uint8_t>(23U + i));

  memset(key, 0xAA, sizeof(key));
  assert(!reader.activeGrant(kTracker, 8U, scope, key));
  for (auto byte : key) assert(byte == 0xAA);
  assert(!reader.activeGrant(kTracker + 1U, 7U, scope, key));

  reader.denyForCurrentBoot();
  assert(!reader.verifiedForLocalGateway(kGateway));
  assert(!reader.activeGrant(kTracker, 7U, scope, key));
  // This is a RAM kill switch, not durable revocation. New boot must
  // reauthenticate the committed snapshot; no local app may mint one.
  GatewayGrantSnapshotReader reboot(nor, verifier);
  assert(reboot.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kReady);
  assert(reboot.activeGrant(kTracker, 7U, scope, key));
  assert(nor.erases == 0U && nor.writes == 0U);
}
void testDuplicateAndReadFailure() {
  Nor nor;
  FakeVerifier verifier;
  makePage(nor, 0U, kGateway, 1U);
  auto* raw = nor.bytes.data();
  placeGrant(raw, 1U, kTracker, 7U, 10U, 5U, 33U);
  uint8_t proof[fmt::kProofBytes]{};
  fakeProof(raw, fmt::kPayloadBytes, proof);
  memcpy(raw + fmt::kPayloadBytes, proof, sizeof(proof));
  be32(raw + fmt::kCrcOffset,
       observation_store_format::crc32(raw, fmt::kCrcOffset));
  GatewayGrantSnapshotReader duplicate(nor, verifier);
  assert(duplicate.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kRejected);

  Nor healthy;
  makePage(healthy, 0U, kGateway, 1U);
  GatewayGrantSnapshotReader reader(healthy, verifier);
  assert(reader.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kReady);
  healthy.read_ok = false;
  GatewayCustodyAckScope scope{};
  uint8_t key[32]{};
  // No unauthenticated post-boot flash read: the verified RAM snapshot is
  // immutable until an explicit security-owner invalidation.
  assert(reader.activeGrant(kTracker, 7U, scope, key));
  assert(key[0] == 17U);
  healthy.bytes[fmt::kHeaderBytes + 24U] = 0xEEU;
  assert(reader.activeGrant(kTracker, 7U, scope, key));
  assert(key[0] == 17U);
  healthy.unreconciled = true;
  assert(!reader.activeGrant(kTracker, 7U, scope, key));
  assert(!reader.verifiedForLocalGateway(kGateway));
}
void testBootGuardIntegration() {
  Nor security_flash;
  Nor product_flash;  // Distinct physical FlashBackend instance in the test.
  FakeVerifier verifier;
  makePage(security_flash, 0U, kGateway, 1U);
  GatewayGrantSnapshotReader reader(security_flash, verifier);
  assert(reader.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kReady);
  ProductCustodyStore custody(product_flash, fmt::kPageCount);
  GatewayProductStartupGuard startup(product_flash, fmt::kPageCount, custody,
                                    reader, kGateway);
  assert(startup.inspectAndMount(true, false, kGateway) ==
         GatewayProductStartupGuard::State::kReady);
  assert(startup.mayServe(true, false, kGateway));
  reader.denyForCurrentBoot();
  assert(!startup.mayServe(true, false, kGateway));
  assert(product_flash.erases == 0U && product_flash.writes == 0U);
  assert(security_flash.erases == 0U && security_flash.writes == 0U);

  Nor invalid_security;
  Nor blank_product;
  GatewayGrantSnapshotReader invalid_reader(invalid_security, verifier);
  assert(invalid_reader.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kEmpty);
  ProductCustodyStore blank_custody(blank_product, fmt::kPageCount);
  GatewayProductStartupGuard blocked(blank_product, fmt::kPageCount,
                                    blank_custody, invalid_reader, kGateway);
  assert(blocked.inspectAndMount(true, false, kGateway) ==
         GatewayProductStartupGuard::State::kUnprovisioned);
  assert(!blocked.mayServe(true, false, kGateway));
}
}  // namespace

int main() {
  testEmptyAndRejected();
  testCommittedRebootAndSelection();
  testDuplicateAndReadFailure();
  testBootGuardIntegration();
  puts("SF5K3Y Gateway signed-grant read-only recovery: PASS (FAKE issuer proof)");
  return 0;
}
