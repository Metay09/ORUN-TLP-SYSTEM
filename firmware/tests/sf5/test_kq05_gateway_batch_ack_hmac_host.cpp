// KQ05 host-only actual SHA256/HMAC-SHA256 batch custody construction.
// Only an explicit HOST_TEST friend may invoke privileged crypto entrypoints.
// NOT proof of Gateway flash custody, Tracker SecurityStore grants or release.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "custody_ack_primitives.h"
#include "kq05_gateway_batch_custody_crypto.h"
#include "tlp_v2_product_secure.h"

namespace orun_tlp {
struct Kq05BatchCryptoHostPeer {
  static bool sign(const GatewayCustodyAckScope& scope,
                   const uint8_t (&key)[32],
                   const Kq05BatchExactView* objects, uint8_t count,
                   uint8_t (&wire)[kq05_gateway_batch_ack::kMaxSize],
                   size_t& len) {
    return Kq05GatewayBatchCustodyCrypto::signAfterDurableReadback(
        scope, key, objects, count, wire, len);
  }
  static bool verify(const uint8_t* wire, size_t size,
                     const GatewayCustodyAckScope& scope,
                     const uint8_t (&key)[32],
                     const Kq05BatchExactView* objects, uint8_t count) {
    return Kq05GatewayBatchCustodyCrypto::verifyAgainstExactRetained(
        wire, size, scope, key, objects, count);
  }
};
} // namespace orun_tlp

namespace {
namespace ack = orun_tlp::kq05_gateway_batch_ack;
using orun_tlp::GatewayCustodyAckScope;
using orun_tlp::Kq05BatchExactView;
using Peer = orun_tlp::Kq05BatchCryptoHostPeer;

void put64(uint8_t* p, uint64_t n) {
  for (size_t i = 0U; i < 8U; ++i)
    p[i] = static_cast<uint8_t>(n >> (56U - 8U*i));
}
void put32(uint8_t* p, uint32_t n) {
  for (size_t i = 0U; i < 4U; ++i)
    p[i] = static_cast<uint8_t>(n >> (24U - 8U*i));
}

GatewayCustodyAckScope scope() {
  GatewayCustodyAckScope s{};
  s.tracker_device_id=UINT64_C(0x0102030405060708);
  s.gateway_device_id=UINT64_C(0x2300230023002301);
  s.key_epoch=7U;
  s.custody_floor=12U;
  s.grant_generation=5U;
  return s;
}

void makeFrame(uint8_t* p, size_t n, uint32_t sequence) {
  assert(n == 100U || n == 84U);
  memset(p, 0U, n);
  p[0]=2U; p[1]=4U; p[2]=2U;
  p[3] = n == 100U ? 1U : 2U;
  p[4]=3U; p[5]=1U;
  p[6] = n == 100U ? 64U : 48U;
  put64(p + 8U, UINT64_C(0x0102030405060708));
  put32(p + 16U, 7U);
  put64(p + 20U, 42U + sequence);
  for (size_t i=28U; i<n-8U; ++i)
    p[i]=static_cast<uint8_t>((i*5U + sequence*7U) & 0xFFU);
  for (size_t i=n-8U; i<n; ++i)
    p[i]=static_cast<uint8_t>(0xA0U + i - (n-8U));
}

struct Frames {
  uint8_t bytes[ack::kMaxObjects][100]{};
  Kq05BatchExactView views[ack::kMaxObjects]{};

  Frames() {
    for (uint8_t i=0U; i<ack::kMaxObjects; ++i) {
      const size_t n=(i & 1U) == 0U ? 100U : 84U;
      makeFrame(bytes[i], n, 1U+i);
      views[i].bytes=bytes[i];
      views[i].size=n;
    }
  }
};

void testAllBatchWidthsAndTamper() {
  uint8_t key[32]{};
  for (size_t i=0U; i<sizeof(key); ++i)
    key[i]=static_cast<uint8_t>(i*7U+11U);
  Frames frames;
  const GatewayCustodyAckScope grant=scope();
  for (uint8_t n=1U; n<=ack::kMaxObjects; ++n) {
    uint8_t wire[ack::kMaxSize]{};
    memset(wire, 0xA5U, sizeof(wire));
    size_t size=999U;
    assert(Peer::sign(grant,key,frames.views,n,wire,size));
    assert(size == ack::kHeaderSize + ack::kRefSize*n + ack::kTagSize);
    for (size_t i=size; i<sizeof(wire); ++i)
      assert(wire[i] == 0xA5U);
    assert(Peer::verify(wire,size,grant,key,frames.views,n));
    assert(!Peer::verify(wire,size,grant,key,frames.views,n-1U));

    // Permuted arrival order yields the same canonical MAC/digest order.
    Kq05BatchExactView reversed[ack::kMaxObjects]{};
    for (uint8_t i=0U;i<n;++i) reversed[i]=frames.views[n-i-1U];
    uint8_t second[ack::kMaxSize]{};
    size_t second_size=0U;
    assert(Peer::sign(grant,key,reversed,n,second,second_size));
    assert(second_size==size && memcmp(second,wire,size)==0);
    assert(Peer::verify(wire,size,grant,key,reversed,n));

    // Every committed object's full digest contributes to this MAC.
    for (uint8_t i=0U;i<n;++i) {
      frames.bytes[i][30U]^=0x80U;
      assert(!Peer::verify(wire,size,grant,key,frames.views,n));
      frames.bytes[i][30U]^=0x80U;
      assert(Peer::verify(wire,size,grant,key,frames.views,n));
    }
    uint8_t tampered[ack::kMaxSize]{};
    memcpy(tampered,wire,size);
    tampered[size-1U]^=0x02U;
    assert(!Peer::verify(tampered,size,grant,key,frames.views,n));
    memcpy(tampered,wire,size);
    tampered[ack::kHeaderSize]^=0x20U;
    assert(!Peer::verify(tampered,size,grant,key,frames.views,n));
    memcpy(tampered,wire,size);
    tampered[24U]^=0x01U;  // custody floor changes; cannot reuse MAC
    assert(!Peer::verify(tampered,size,grant,key,frames.views,n));

    GatewayCustodyAckScope different=grant;
    ++different.gateway_device_id;
    assert(!Peer::verify(wire,size,different,key,frames.views,n));
    different=grant;
    ++different.grant_generation;
    assert(!Peer::verify(wire,size,different,key,frames.views,n));
    different=grant;
    ++different.key_epoch;
    assert(!Peer::verify(wire,size,different,key,frames.views,n));
    key[0U]^=1U;
    assert(!Peer::verify(wire,size,grant,key,frames.views,n));
    key[0U]^=1U;
    assert(Peer::verify(wire,size,grant,key,frames.views,n));
  }
}

void testFailClosedNoOutputOrMutableState() {
  Frames frames;
  uint8_t key[32]{};
  for (size_t i=0;i<32U;++i) key[i]=static_cast<uint8_t>(i+5U);
  const GatewayCustodyAckScope grant=scope();
  uint8_t output[ack::kMaxSize]{};
  uint8_t sentinel[ack::kMaxSize]{};
  memset(sentinel,0xA5U,sizeof(sentinel));

  auto rejected=[&](const GatewayCustodyAckScope& s,
                    const Kq05BatchExactView* v, uint8_t n) {
    memcpy(output,sentinel,sizeof(output));
    size_t written=200U;
    assert(!Peer::sign(s,key,v,n,output,written));
    assert(written==200U);
    assert(memcmp(output,sentinel,sizeof(output))==0);
  };
  rejected(grant,nullptr,1U);
  rejected(grant,frames.views,0U);
  rejected(grant,frames.views,6U);
  GatewayCustodyAckScope bad=grant;
  bad.gateway_device_id=0U;
  rejected(bad,frames.views,2U);
  bad=grant;bad.grant_generation=UINT32_MAX;
  rejected(bad,frames.views,2U);
  bad=grant;bad.custody_floor=UINT32_MAX;
  rejected(bad,frames.views,2U);

  Kq05BatchExactView two[2] = {frames.views[0],frames.views[0]};
  rejected(grant,two,2U); // same exact object twice
  two[0] = frames.views[0]; two[1] = frames.views[1];
  two[1].size = 73U;
  rejected(grant,two,2U);
  two[1] = frames.views[1]; two[1].bytes = nullptr;
  rejected(grant,two,2U);
  two[1] = frames.views[1];
  frames.bytes[1][4U]=0U; // no custody request, cannot mint
  rejected(grant,two,2U);
  frames.bytes[1][4U]=3U;
  frames.bytes[1][8U]^=1U; // different Tracker ID
  rejected(grant,two,2U);
  frames.bytes[1][8U]^=1U;
  frames.bytes[1][19U]^=1U; // wrong epoch
  rejected(grant,two,2U);
  frames.bytes[1][19U]^=1U;

  uint8_t wire[ack::kMaxSize]{};
  size_t size=0U;
  assert(Peer::sign(grant,key,frames.views,5U,wire,size));
  assert(Peer::verify(wire,size,grant,key,frames.views,5U));
  // A same-size but different exact retained group is NOT authorized.
  Kq05BatchExactView missing[ack::kMaxObjects]{};
  memcpy(missing,frames.views,sizeof(missing));
  missing[4]=frames.views[0];
  assert(!Peer::verify(wire,size,grant,key,missing,5U));
}

} // namespace

int main() {
  testAllBatchWidthsAndTamper();
  testFailClosedNoOutputOrMutableState();
  puts("KQ05 Gateway batch exact 1..5 SHA256 + scoped HMAC and tamper rejection: PASS (HOST CRYPTO ONLY)");
  puts("NO VERIFIED DURABLE GATEWAY OWNER, NO TRACKER FLASH RELEASE, NO LORA TX");
}
