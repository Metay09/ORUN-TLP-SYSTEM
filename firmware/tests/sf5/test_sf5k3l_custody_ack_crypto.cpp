// SF5K3L: actual SecurityStore v3 grant + production ACK verifier core,
// checked by a distinct OpenSSL host crypto implementation against
// independently derived Python hashlib/HMAC fixed vectors.
// No RF activation and NO durable observation release.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "custody_ack_primitives.h"
#include "kq05_tracker_batch_ack_verifier.h"

#define main sf5k3m_previous_main
#include "test_sf5k3m_real_custody_release.cpp"
#undef main

namespace orun_tlp {
struct Kq05TrackerBatchVerifyHostPeer {
  static uint32_t sequence(
      const AuthenticatedKq05TrackerBatchAck& proof) {
    return proof.identities_[0].sequence;
  }

  static void seed(AuthenticatedKq05TrackerBatchAck& proof) {
    proof.identities_[0].sequence = 99U;
    proof.verified_ = false;
  }
};
}

namespace {
using Verdict = Kq05TrackerBatchVerifyResult;
using Peer = Kq05TrackerBatchVerifyHostPeer;

bool hex(const char* input,uint8_t* out,size_t size) {
  const auto nibble=[](char c)->int {
    if(c>='0' && c<='9')return c-'0';
    if(c>='a' && c<='f')return c-'a'+10;
    return -1;
  };
  for(size_t i=0;i<size;++i) {
    const int a=nibble(input[2U*i]);
    const int b=nibble(input[2U*i+1U]);
    if(a<0 || b<0)return false;
    out[i]=static_cast<uint8_t>((a<<4)|b);
  }
  return input[2U*size]=='\0';
}

constexpr char kOracleAck[] = "020a01010102030405060708230023002300230100000007000000000000000192453945bd2224e1cb275990b721fe660c8f0e2cde31e463";
constexpr char kOracleDigest[] =
  "92453945bd2224e1e9d462cb964b6fdc1e2018bafbe765cd4ac9ebbfc57aef00";
constexpr char kOracleKey[] =
  "b3b50abe4a260946f7a1e13361e742504736f11df4dfd3b1852e5ae242d5b04f";

void testIndependentCryptoOracle() {
  uint8_t frame[tlp::kPeriodicFrameSize]{};
  osf::RecordIdentity id{};
  id.incarnation = 100U;
  id.sequence = 8U;
  osc::ExactObject obj = makeObject(id);
  memcpy(frame,obj.object,sizeof(frame));
  uint8_t digest[32]{};
  uint8_t expected_digest[32]{};
  assert(hex(kOracleDigest,expected_digest,sizeof(expected_digest)));
  assert(orun_tlp::custody_crypto::sha256(frame,sizeof(frame),digest));
  assert(memcmp(digest,expected_digest,32U)==0);
  uint8_t info[57]{};
  const char prefix[]="ORUN-TLP-V2-GW-CUSTODY-ACK-v1";
  memcpy(info,prefix,sizeof(prefix)-1U);
  uint8_t fields[28]{};
  uint8_t wire[56]{};
  assert(hex(kOracleAck,wire,sizeof(wire)));
  memcpy(fields,wire+4U,8U);
  memcpy(fields+8U,wire+20U,4U);
  memcpy(fields+12U,wire+12U,8U);
  memcpy(fields+20U,wire+24U,8U);
  memcpy(info+sizeof(prefix)-1U,fields,sizeof(fields));
  const sf::Credential credential_=credential();
  uint8_t key[32]{};
  uint8_t expected_key[32]{};
  assert(hex(kOracleKey,expected_key,sizeof(expected_key)));
  assert(orun_tlp::custody_crypto::hkdfSha256(
      credential_.credential_id,credential_.k_root,
      info,sizeof(info),key));
  assert(memcmp(key,expected_key,sizeof(key))==0);
  const char domain[]="ORUN-KQ05-GW-BATCH-ACK-V1";
  uint8_t input[sizeof(domain)-1U+40U+32U]{};
  memcpy(input,domain,sizeof(domain)-1U);
  memcpy(input+sizeof(domain)-1U,wire,40U);
  memcpy(input+sizeof(domain)-1U+40U,digest,32U);
  uint8_t mac[32]{};
  assert(orun_tlp::custody_crypto::hmacSha256Bounded(key,input,sizeof(input),mac));
  assert(memcmp(mac,wire+40U,16U)==0);
}

void testRealStoreAndMac() {
  Flash flash;
  SecurityStore security(flash, flash);
  provisionV3(flash, security);

  ProductFlash product_flash;
  Source source;
  ProductRecordStore records(
      product_flash, kProductPages, &source);

  records.begin(kDevice, product_region::Jobs{false,true});
  assert(records.mounted());

  const auto id = append(records);
  auto exact = makeObject(id);
  cache(records, exact);

  Kq05TrackerBatchAckVerifier verifier(security);

  uint8_t ack[56]{};
  assert(hex(kOracleAck, ack, sizeof(ack)));

  AuthenticatedKq05TrackerBatchAck proof{};
  Peer::seed(proof);

  auto verify = [&]() {
    return verifier.verifyFromStore(
        ack, sizeof(ack), records, proof);
  };

  assert(verify() == Verdict::kNoDurableGrant);
  assert(Peer::sequence(proof) == 99U);

  assert(Seam::grant(security, 0U, kGateA));
  assert(verify() == Verdict::kNoDurableGrant);

  settle(security);
  assert(verify() == Verdict::kNoDurableGrant);

  bool applied = false;
  assert(Seam::take(security, applied) && applied);

  assert(verify() == Verdict::kAuthenticated);
  assert(Peer::sequence(proof) == id.sequence);
  assertRetained(records, id, false);

  // Even a correct MAC must not authenticate an
  // ambiguous exact object belonging to two records.
  {
    ProductFlash ambiguous_flash;
    Source ambiguous_source;
    ProductRecordStore ambiguous(
        ambiguous_flash, kProductPages,
        &ambiguous_source);

    ambiguous.begin(
        kDevice, product_region::Jobs{false,true});

    assert(ambiguous.mounted());

    const auto first = append(ambiguous);
    const auto second = append(ambiguous);

    cache(ambiguous, makeObject(first));
    cache(ambiguous, makeObject(second));

    assert(verifier.verifyFromStore(
        ack, sizeof(ack), ambiguous, proof) ==
        Verdict::kNoUniqueObject);

    assert(Peer::sequence(proof) == id.sequence);
    assertRetained(ambiguous, first, false);
    assertRetained(ambiguous, second, false);
  }

  // A different retained ciphertext cannot satisfy
  // the original ACK, despite having the same scope.
  {
    ProductFlash altered_flash;
    Source altered_source;
    ProductRecordStore altered(
        altered_flash, kProductPages, &altered_source);

    altered.begin(
        kDevice, product_region::Jobs{false,true});

    assert(altered.mounted());

    auto object = makeObject(append(altered));
    object.object[99U] ^= 1U;

    cache(altered, object);

    assert(verifier.verifyFromStore(
        ack, sizeof(ack), altered, proof) !=
        Verdict::kAuthenticated);
  }

  // Reject every single-byte mutation of the wire.
  for (size_t i = 0; i < sizeof(ack); ++i) {
    ack[i] ^= 1U;
    assert(verify() != Verdict::kAuthenticated);
    ack[i] ^= 1U;
  }

  assert(Peer::sequence(proof) == id.sequence);

  assert(Seam::revoke(security, 0U, kGateA));
  assert(verify() == Verdict::kNoDurableGrant);

  settle(security);
  assert(Seam::take(security, applied) && applied);
  assert(verify() == Verdict::kNoDurableGrant);

  assert(Seam::grant(security, 1U, kGateA));
  settle(security);
  assert(Seam::take(security, applied) && applied);

  assert(verify() == Verdict::kNoDurableGrant);

  SecurityStore reboot(flash, flash);
  assert(reboot.begin(
      DeviceIdentity::fromLegacyUint64(kDevice)));

  settle(reboot);

  Kq05TrackerBatchAckVerifier restarted(reboot);

  assert(restarted.verifyFromStore(
      ack, sizeof(ack), records, proof) ==
      Verdict::kNoDurableGrant);

  assertRetained(records, id, false);
}
} // namespace

int main(){
  testIndependentCryptoOracle();
  testRealStoreAndMac();
  puts("SF5K3L actual SecurityStore v3 scoped SHA256/HKDF/HMAC verifier: PASS");
  return 0;
}
