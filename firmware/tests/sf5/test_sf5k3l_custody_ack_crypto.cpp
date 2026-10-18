// SF5K3L: actual SecurityStore v3 grant + production ACK verifier core,
// checked by a distinct OpenSSL host crypto implementation against
// independently derived Python hashlib/HMAC fixed vectors.
// No RF activation and NO durable observation release.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "custody_ack_verifier.h"
#include "custody_ack_primitives.h"
#include "tlp_v2_product_secure.h"

#define main sf5k3j_policy_previous_main
#include "test_sf5k3j_real_security_v3_policy.cpp"
#undef main

namespace orun_tlp {
struct CustodyAckVerifierHostPeer {
  static CustodyAckVerifyResult check(CustodyAckVerifier& verifier,
      const uint8_t* ack,size_t length,
      const observation_store_control::ExactObject* cached,
      uint16_t count,AuthenticatedCustodyAck& result) {
    uint16_t selected=UINT16_MAX;
    const auto verdict=verifier.verifyCandidates(
        ack,length,cached,count,result,selected);
    if (verdict==CustodyAckVerifyResult::kAuthenticated)
      assert(selected<count);
    return verdict;
  }
  static observation_store_format::RecordIdentity identity(
      const AuthenticatedCustodyAck& proof) {
    return proof.identity_;
  }
  static void seed(AuthenticatedCustodyAck& proof) {
    proof.identity_.incarnation=UINT64_C(0x123456789);
    proof.identity_.sequence=99U;
  }
};
}

namespace {
namespace tlp=orun_tlp::tlp;
namespace osc=orun_tlp::observation_store_control;
using Verdict=orun_tlp::CustodyAckVerifyResult;
using Peer=orun_tlp::CustodyAckVerifierHostPeer;

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

constexpr char kOracleAck[] =
  "02050164010203040506070823002300230023010000000700000000"
  "0000000192453945bd2224e144572044decab3402105011f5844a453";
constexpr char kOracleDigest[] =
  "92453945bd2224e1e9d462cb964b6fdc1e2018bafbe765cd4ac9ebbfc57aef00";
constexpr char kOracleKey[] =
  "b3b50abe4a260946f7a1e13361e742504736f11df4dfd3b1852e5ae242d5b04f";

void makeObject(osc::ExactObject& object) {
  tlp::ProductSecureFrame p{};
  p.app_family=tlp::kProductAppFamilyPeriodic;
  p.path_flags=3U;
  p.security_profile=tlp::kProductSecurityProfileRootAeadV1;
  p.ciphertext_len=tlp::kPeriodicPlaintextSize;
  p.device_id=kDevice;
  p.key_epoch=7U;
  p.security_counter=42U;
  for(size_t i=0;i<tlp::kPeriodicPlaintextSize;++i)
    p.ciphertext[i]=static_cast<uint8_t>(i*5U+7U);
  for(size_t i=0;i<tlp::kProductSecureTagSize;++i)
    p.tag[i]=static_cast<uint8_t>(0xA0U+i);
  object.identity.incarnation=100U;
  object.identity.sequence=8U;
  object.record_kind=orun_tlp::observation_store_format::RecordKind::kPeriodic;
  object.object_size=tlp::kPeriodicFrameSize;
  assert(tlp::encodeProductSecureFrame(p,object.object,sizeof(object.object))==
         tlp::ProductSecureCodecResult::kOk);
}

void testIndependentCryptoOracle() {
  uint8_t frame[tlp::kPeriodicFrameSize]{};
  osc::ExactObject obj{};
  makeObject(obj);
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
  uint8_t input[102]{};
  const char domain[]="ORUN-TLP-V2-CUSTODY-ACK-MAC-v1";
  memcpy(input,domain,sizeof(domain)-1U);
  memcpy(input+sizeof(domain)-1U,wire,40U);
  memcpy(input+sizeof(domain)-1U+40U,digest,32U);
  uint8_t mac[32]{};
  assert(orun_tlp::custody_crypto::hmacSha256(key,input,sizeof(input),mac));
  assert(memcmp(mac,wire+40U,16U)==0);
}

void testRealStoreAndMac() {
  Flash flash;
  SecurityStore s(flash,flash);
  provisionV3(flash,s);
  CustodyAckVerifier verifier(s);
  uint8_t ack[56]{};
  assert(hex(kOracleAck,ack,sizeof(ack)));
  osc::ExactObject cached[4]{};
  makeObject(cached[0]);
  AuthenticatedCustodyAck proof{};
  Peer::seed(proof);
  assert(Peer::check(verifier,ack,sizeof(ack),cached,1U,proof)==
         Verdict::kNoDurableGrant);
  assert(Peer::identity(proof).sequence==99U); // no output on failure

  assert(Seam::grant(s,0U,kGateA));
  assert(Peer::check(verifier,ack,sizeof(ack),cached,1U,proof)==
         Verdict::kNoDurableGrant);
  settle(s);
  assert(Peer::check(verifier,ack,sizeof(ack),cached,1U,proof)==
         Verdict::kNoDurableGrant); // unread policy success
  bool applied=false;
  assert(Seam::take(s,applied)&&applied);
  assert(Peer::check(verifier,ack,sizeof(ack),cached,1U,proof)==
         Verdict::kAuthenticated);
  assert(Peer::identity(proof).incarnation==100U &&
         Peer::identity(proof).sequence==8U);

  // Complete output remains opaque and may not itself release a record.
  osc::ExactObject alternative=cached[0];
  alternative.identity.sequence=9U;
  cached[1]=alternative;
  assert(Peer::check(verifier,ack,sizeof(ack),cached,2U,proof)==
         Verdict::kNoUniqueObject); // even a valid MAC + 64-bit hint
  assert(Peer::identity(proof).sequence==8U);
  cached[1]=osc::ExactObject{};
  cached[0].object[99U]^=1U;
  assert(Peer::check(verifier,ack,sizeof(ack),cached,1U,proof)!=
         Verdict::kAuthenticated);
  cached[0].object[99U]^=1U;

  for(size_t i=0U;i<sizeof(ack);++i) {
    uint8_t corrupted[56]{};
    memcpy(corrupted,ack,sizeof(corrupted));
    corrupted[i]^=1U;
    assert(Peer::check(verifier,corrupted,sizeof(corrupted),
                       cached,1U,proof)!=Verdict::kAuthenticated);
  }
  assert(Peer::identity(proof).sequence==8U);

  assert(Seam::revoke(s,0U,kGateA));
  assert(Peer::check(verifier,ack,sizeof(ack),cached,1U,proof)==
         Verdict::kNoDurableGrant);
  settle(s);
  assert(Seam::take(s,applied)&&applied);
  assert(Peer::check(verifier,ack,sizeof(ack),cached,1U,proof)==
         Verdict::kNoDurableGrant);
  assert(Seam::grant(s,1U,kGateA));
  settle(s);
  assert(Seam::take(s,applied)&&applied);
  assert(Peer::check(verifier,ack,sizeof(ack),cached,1U,proof)==
         Verdict::kNoDurableGrant); // old generation, even valid old MAC

  SecurityStore reboot(flash,flash);
  assert(reboot.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  settle(reboot);
  CustodyAckVerifier restarted(reboot);
  assert(Peer::check(restarted,ack,sizeof(ack),cached,1U,proof)==
         Verdict::kNoDurableGrant);
}
} // namespace

#ifndef ORUN_SF5K3M_HOST_TEST
int main(){
  testIndependentCryptoOracle();
  testRealStoreAndMac();
  puts("SF5K3L actual SecurityStore v3 scoped SHA256/HKDF/HMAC verifier: PASS");
  return 0;
}
#endif // ORUN_SF5K3M_HOST_TEST
