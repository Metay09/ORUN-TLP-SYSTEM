// SF5K3Z: REAL OpenSSL Ed25519 issuer signature and exact-revision gate,
// against the actual SF5K3Y read-only OGK1 + OCQ2 startup classes.
// OpenSSL is HOST-ONLY. There is no embedded issuer key, physical secure
// checkpoint, Gateway field enrollment, production signing or actual LoRa.
#include <openssl/evp.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gateway_anchored_grant_source.h"

#define main sf5k3y_imported_main
#include "test_sf5k3y_gateway_grant_reader.cpp"
#undef main

namespace {
struct Issuer {
  EVP_PKEY* private_key = nullptr;
  uint8_t public_key[32]{};

  Issuer() {
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    assert(context);
    assert(EVP_PKEY_keygen_init(context) == 1);
    assert(EVP_PKEY_keygen(context, &private_key) == 1);
    EVP_PKEY_CTX_free(context);
    size_t pub_size = sizeof(public_key);
    assert(EVP_PKEY_get_raw_public_key(private_key, public_key, &pub_size) == 1);
    assert(pub_size == sizeof(public_key));
  }
  ~Issuer() { EVP_PKEY_free(private_key); }
  Issuer(const Issuer&) = delete;
  Issuer& operator=(const Issuer&) = delete;

  void sign(Nor& flash, uint16_t page) const {
    uint8_t* raw = flash.bytes.data() + page * fmt::kPageBytes;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    assert(context);
    assert(EVP_DigestSignInit(context, nullptr, nullptr, nullptr,
                              private_key) == 1);
    uint8_t signature[fmt::kProofBytes]{};
    size_t length = sizeof(signature);
    assert(EVP_DigestSign(context, signature, &length, raw,
                          fmt::kPayloadBytes) == 1);
    assert(length == sizeof(signature));
    memcpy(raw + fmt::kPayloadBytes, signature, sizeof(signature));
    be32(raw + fmt::kCrcOffset,
         observation_store_format::crc32(raw, fmt::kCrcOffset));
    EVP_MD_CTX_free(context);
  }
};

struct RealEd25519Verifier final : GatewayEnrollmentProofVerifier {
  uint8_t issuer_key[32]{};
  explicit RealEd25519Verifier(const uint8_t (&key)[32]) {
    memcpy(issuer_key, key, sizeof(issuer_key));
  }
  bool verify(const uint8_t* data, size_t length,
              const uint8_t (&proof)[fmt::kProofBytes]) const override {
    if (!data || length != fmt::kPayloadBytes) return false;
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, issuer_key, sizeof(issuer_key));
    if (!key) return false;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    bool valid = false;
    if (context &&
        EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1) {
      valid = EVP_DigestVerify(context, proof, sizeof(proof),
                               data, length) == 1;
    }
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return valid;
  }
};

struct RevisionAnchor final : GatewayGrantRevisionAnchor {
  bool trusted = true;
  uint64_t revision = 0U;
  uint64_t expected_gateway = kGateway;
  mutable unsigned reads = 0U;
  bool currentRevision(uint64_t gateway, uint64_t& result) const override {
    ++reads;
    if (!trusted || gateway != expected_gateway || revision == 0U)
      return false;
    result = revision;
    return true;
  }
};

void issuerSignatureAndRollback() {
  Nor security;
  Nor product;
  Issuer issuer;  // Ephemeral TEST issuer only. Never embedded in firmware.
  makePage(security, 0U, kGateway, 6U);
  makePage(security, 1U, kGateway, 7U);
  issuer.sign(security, 0U);
  issuer.sign(security, 1U);
  RealEd25519Verifier verifier(issuer.public_key);

  GatewayGrantSnapshotReader reader(security, verifier);
  assert(reader.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kReady);
  assert(reader.activeGeneration() == 7U);
  RevisionAnchor anchor;
  anchor.revision = 7U;
  GatewayAnchoredGrantSource authoritative(reader, anchor);
  assert(authoritative.verifiedForLocalGateway(kGateway));

  GatewayCustodyAckScope scope{};
  uint8_t key[32]{};
  assert(authoritative.activeGrant(kTracker, 7U, scope, key));
  assert(scope.gateway_device_id == kGateway && scope.key_epoch == 7U);
  assert(key[0] == 17U);

  ProductCustodyStore custody(product, fmt::kPageCount);
  GatewayProductStartupGuard startup(product, fmt::kPageCount,
                                    custody, authoritative, kGateway);
  assert(startup.inspectAndMount(true, false, kGateway) ==
         GatewayProductStartupGuard::State::kReady);
  assert(startup.mayServe(true, false, kGateway));

  // Correctly signed but STALE snapshot must not serve if checkpoint has
  // moved forward. Future lower-revision anchor change also fails closed.
  anchor.revision = 8U;
  assert(!startup.mayServe(true, false, kGateway));
  memset(key, 0xAA, sizeof(key));
  assert(!authoritative.activeGrant(kTracker, 7U, scope, key));
  for (auto b : key) assert(b == 0xAA);
  anchor.revision = 6U;
  assert(!authoritative.verifiedForLocalGateway(kGateway));
  anchor.revision = 7U;
  assert(startup.mayServe(true, false, kGateway));
  anchor.trusted = false;
  assert(!startup.mayServe(true, false, kGateway));
  anchor.trusted = true;

  // Remove newer signed snapshot: an old correctly signed snapshot still
  // exists, but the independent current revision 7 blocks its activation.
  memset(security.bytes.data() + fmt::kPageBytes,
         0xFF, fmt::kPageBytes);
  GatewayGrantSnapshotReader older(security, verifier);
  assert(older.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kReady);
  assert(older.activeGeneration() == 6U);
  GatewayAnchoredGrantSource rejected(older, anchor);
  assert(!rejected.verifiedForLocalGateway(kGateway));
  assert(!rejected.activeGrant(kTracker, 7U, scope, key));
  assert(security.writes == 0U && security.erases == 0U);
  assert(product.writes == 0U && product.erases == 0U);
}

void rejectForgedOrWrongIssuer() {
  Issuer issuer;
  Issuer other;
  Nor forged;
  makePage(forged, 0U, kGateway, 1U);
  issuer.sign(forged, 0U);
  // Tamper the grant after real signing. Recompute unkeyed CRC to demonstrate
  // that CRC cannot replace authentication.
  forged.bytes[fmt::kHeaderBytes + 24U] ^= 0x37U;
  be32(forged.bytes.data() + fmt::kCrcOffset,
       observation_store_format::crc32(forged.bytes.data(),
                                       fmt::kCrcOffset));
  RealEd25519Verifier verifier(issuer.public_key);
  GatewayGrantSnapshotReader no_signature(forged, verifier);
  assert(no_signature.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kRejected);

  Nor wrong_issuer;
  makePage(wrong_issuer, 0U, kGateway, 1U);
  issuer.sign(wrong_issuer, 0U);
  RealEd25519Verifier wrong(other.public_key);
  GatewayGrantSnapshotReader other_authority(wrong_issuer, wrong);
  assert(other_authority.begin(kGateway, true, false) ==
         GatewayGrantSnapshotReader::Status::kRejected);
}

}  // namespace

// Other host suites can reuse the issuer proof fixture without defining
// a second main(). The standard SF5K3Z test is unchanged by default.
#ifndef ORUN_SF5K3AB_REUSE_SF5K3Z_FIXTURE
int main() {
  issuerSignatureAndRollback();
  rejectForgedOrWrongIssuer();
  puts("SF5K3Z real Ed25519 issuer signature + exact rollback checkpoint: PASS (HOST ONLY)");
  return 0;
}
#endif  // ORUN_SF5K3AB_REUSE_SF5K3Z_FIXTURE
