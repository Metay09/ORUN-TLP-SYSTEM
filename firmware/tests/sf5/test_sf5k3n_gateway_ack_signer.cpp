// Isolated SF5K3N host check for the production byte-exact Gateway ACK signer.
// Test-only peer is the ONLY test bypass of the private durable-owner gate.
// No Gateway persistence or RF activation is claimed here.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "gateway_custody_ack_signer.h"
#include "custody_ack_primitives.h"

namespace orun_tlp {
struct GatewayCustodyAckSignerTestPeer {
  static bool sign(const uint8_t* object, size_t object_len,
                   const GatewayCustodyAckScope& scope,
                   const uint8_t (&key)[32],
                   uint8_t (&output)[custody_ack::kWireSize]) {
    return GatewayCustodyAckSigner::signAfterDurableAcceptance(
        object, object_len, scope, key, output);
  }
};
}  // namespace orun_tlp

namespace {
using namespace orun_tlp;

constexpr char kExpectedAck[] =
    "02050164010203040506070823002300230023010000000700000000"
    "0000000192453945bd2224e144572044decab3402105011f5844a453";
constexpr char kExpectedSha[] =
    "92453945bd2224e1e9d462cb964b6fdc1e2018bafbe765cd4ac9ebbfc57aef00";
constexpr char kExpectedEventAck[] =
    "02050154010203040506070823002300230023010000000700000000"
    "00000001bf62c359077620ad82210adadc9b12c72f6467542172aaa8";
constexpr char kScopeKey[] =
    "b3b50abe4a260946f7a1e13361e742504736f11df4dfd3b1852e5ae242d5b04f";

uint8_t nybble(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c-'0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c-'a'+10);
  assert(false);
  return 0U;
}

template <size_t N>
void fromHex(const char* source, uint8_t (&dest)[N]) {
  assert(strlen(source) == N*2U);
  for (size_t i=0U;i<N;++i)
    dest[i]=static_cast<uint8_t>((nybble(source[i*2U])<<4U)|nybble(source[i*2U+1U]));
}

GatewayCustodyAckScope testScope() {
  GatewayCustodyAckScope s{};
  s.tracker_device_id=UINT64_C(0x0102030405060708);
  s.gateway_device_id=UINT64_C(0x2300230023002301);
  s.key_epoch=7U;
  s.custody_floor=0U;
  s.grant_generation=1U;
  return s;
}

void testAll() {
  // Independent Python hashlib/HMAC oracle fixture, identical to SF5K3L.
  uint8_t object[100]{};
  object[0]=0x02U; object[1]=0x04U; object[2]=0x02U;
  object[3]=0x01U; object[4]=0x03U; object[5]=0x01U;
  object[6]=64U;
  for (unsigned i=0;i<8U;++i)
    object[8U+i]=static_cast<uint8_t>(i+1U);
  object[19U]=7U;  // epoch BE32
  object[27U]=42U; // security counter BE64
  for (unsigned i=0;i<64U;++i)
    object[28U+i]=static_cast<uint8_t>(i*5U+7U);
  for (unsigned i=0;i<8U;++i)
    object[92U+i]=static_cast<uint8_t>(0xa0U+i);

  uint8_t digest[32]{};
  uint8_t expected_digest[32]{};
  fromHex(kExpectedSha,expected_digest);
  assert(custody_crypto::sha256(object,sizeof(object),digest));
  assert(memcmp(digest,expected_digest,sizeof(digest))==0);

  uint8_t key[32]{};
  fromHex(kScopeKey,key);
  uint8_t expected_ack[56]{};
  fromHex(kExpectedAck,expected_ack);
  uint8_t out[56]{};
  GatewayCustodyAckScope scope=testScope();
  auto sign=[&]() {
    return GatewayCustodyAckSignerTestPeer::sign(
        object,sizeof(object),scope,key,out);
  };
  assert(sign());
  assert(memcmp(expected_ack,out,sizeof(out))==0);
  custody_ack::Header header{};
  assert(custody_ack::decodeHeader(out,sizeof(out),header));
  assert(header.relay_allowed && header.object_len==sizeof(object));

  // Byte-for-byte idempotence; duplicate durable object re-ACK can reuse tag.
  uint8_t original[56]{};
  memcpy(original,out,sizeof(out));
  assert(sign() && memcmp(out,original,sizeof(out))==0);

  // Any rejected input MUST leave the entire caller output unchanged.
  auto rejected=[&](const uint8_t* obj,size_t n) {
    memset(out,0xA5U,sizeof(out));
    assert(!GatewayCustodyAckSignerTestPeer::sign(obj,n,scope,key,out));
    for (uint8_t b:out) assert(b==0xA5U);
  };
  rejected(nullptr,sizeof(object));
  rejected(object,0U);
  rejected(object,83U);
  rejected(object,99U);
  rejected(object,SIZE_MAX);
  scope.tracker_device_id=0U; rejected(object,sizeof(object)); scope=testScope();
  scope.gateway_device_id=0U; rejected(object,sizeof(object)); scope=testScope();
  scope.key_epoch=UINT32_MAX; rejected(object,sizeof(object)); scope=testScope();
  scope.custody_floor=UINT32_MAX; rejected(object,sizeof(object)); scope=testScope();
  scope.grant_generation=0U; rejected(object,sizeof(object)); scope=testScope();
  scope.grant_generation=UINT32_MAX; rejected(object,sizeof(object)); scope=testScope();
  scope.tracker_device_id++; rejected(object,sizeof(object)); scope=testScope();
  scope.key_epoch++; rejected(object,sizeof(object)); scope=testScope();
  object[4U]=0x01U; rejected(object,sizeof(object)); object[4U]=0x03U;
  object[7U]=0x01U; rejected(object,sizeof(object)); object[7U]=0x00U;
  object[27U]=0U; rejected(object,sizeof(object)); object[27U]=42U;
  object[6U]=48U; rejected(object,sizeof(object)); object[6U]=64U;

  // An exact-object byte change still gives a valid *different* MAC, never
  // the original object's ACK. The consumer must match the full SHA256.
  object[50U]^=0x40U;
  assert(sign());
  assert(memcmp(out,original,sizeof(out))!=0);
  object[50U]^=0x40U;
  key[0]^=0x40U;
  assert(sign());
  assert(memcmp(out,original,sizeof(out))!=0);

  // Frozen EVENT (84 B) family uses the same authenticated envelope, with
  // an independently derived Python HMAC oracle distinct from PERIODIC.
  uint8_t event[84]{};
  memcpy(event,object,28U);
  event[3U]=0x02U;
  event[6U]=48U;
  for (unsigned i=0U;i<48U;++i)
    event[28U+i]=static_cast<uint8_t>(i*5U+7U);
  for (unsigned i=0U;i<8U;++i)
    event[76U+i]=static_cast<uint8_t>(0xA0U+i);
  key[0]^=0x40U;  // restore the original oracle key
  uint8_t expected_event_ack[56]{};
  fromHex(kExpectedEventAck,expected_event_ack);
  assert(GatewayCustodyAckSignerTestPeer::sign(
      event,sizeof(event),scope,key,out));
  assert(memcmp(out,expected_event_ack,sizeof(out))==0);
  assert(out[3U]==84U && out[2U]==custody_ack::kRelayAllowed);

  puts("SF5K3N private Gateway custody ACK signer / 2 frozen oracles / fail-closed: PASS");
}
}  // namespace

int main() { testAll(); }
