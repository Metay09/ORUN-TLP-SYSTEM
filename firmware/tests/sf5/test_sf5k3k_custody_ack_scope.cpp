// SF5K3K: ACTUAL SecurityStore durable-grant scope, untrusted 0x05
// codec and protected-frame shape. No MAC, RF authorizer or release path.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "custody_ack.h"
#include "tlp_v2_product_secure.h"

// Reuse the real SecurityStore/NOR fixture, not a second divergent model.
// Do NOT invoke the previous main: this target runs only new K assertions.
#define main sf5k3j_policy_previous_main
#include "test_sf5k3j_real_security_v3_policy.cpp"
#undef main

namespace {
namespace ca = orun_tlp::custody_ack;
namespace tlp = orun_tlp::tlp;

void put32(uint8_t* o, uint32_t x) {
  for (unsigned i = 0U; i < 4U; ++i)
    o[i] = static_cast<uint8_t>(x >> (24U-8U*i));
}
void put64(uint8_t* o, uint64_t x) {
  for (unsigned i = 0U; i < 8U; ++i)
    o[i] = static_cast<uint8_t>(x >> (56U-8U*i));
}

void mkWire(uint8_t (&wire)[ca::kWireSize], uint64_t gateway,
            uint32_t generation, uint32_t floor = 0U,
            uint8_t object_len = 100U) {
  memset(wire, 0U, sizeof(wire));
  wire[0] = ca::kVersion;
  wire[1] = ca::kType;
  wire[2] = ca::kRelayAllowed;
  wire[3] = object_len;
  put64(wire+4U, kDevice);
  put64(wire+12U, gateway);
  put32(wire+20U, 7U);
  put32(wire+24U, floor);
  put32(wire+28U, generation);
  for (unsigned i=0U;i<8U;++i) wire[32U+i]=static_cast<uint8_t>(i+2U);
  for (unsigned i=0U;i<16U;++i) wire[40U+i]=static_cast<uint8_t>(i+9U);
}

void testExactHeaderAndObjectShape() {
  uint8_t wire[ca::kWireSize]{};
  mkWire(wire,kGateA,1U);
  ca::Header header{};
  assert(ca::decodeHeader(wire,sizeof(wire),header));
  assert(header.tracker_device_id==kDevice);
  assert(header.gateway_device_id==kGateA);
  assert(header.key_epoch==7U && header.custody_floor==0U &&
         header.grant_generation==1U && header.object_len==100U &&
         header.relay_allowed);
  ca::Header sentinel{};
  sentinel.gateway_device_id=0xABCDEFU;
  for (unsigned i=0U;i<56U;++i) {
    // No structural parser may treat an arbitrary untrusted byte as proof
    // of authenticity. Reserved/type/length fields MUST be rejected.
    uint8_t altered[ca::kWireSize]{};
    memcpy(altered,wire,sizeof(wire));
    if (i==0U || i==1U || i==2U || i==3U) {
      altered[i] = (i==2U ? 0x80U : 0xFFU);
      assert(!ca::decodeHeader(altered,sizeof(altered),sentinel));
      assert(sentinel.gateway_device_id==0xABCDEFU);
    }
  }
  assert(!ca::decodeHeader(wire,55U,sentinel));
  assert(!ca::decodeHeader(wire,57U,sentinel));
  assert(!ca::decodeHeader(nullptr,56U,sentinel));

  uint8_t protected_frame[tlp::kPeriodicFrameSize]{};
  tlp::ProductSecureFrame frame{};
  frame.app_family=tlp::kProductAppFamilyPeriodic;
  frame.path_flags=tlp::kProductPathFlagCustodyRequested;
  frame.ciphertext_len=tlp::kPeriodicPlaintextSize;
  frame.device_id=kDevice;
  frame.key_epoch=7U;
  frame.security_counter=42U;
  assert(tlp::encodeProductSecureFrame(
      frame,protected_frame,sizeof(protected_frame))==
      tlp::ProductSecureCodecResult::kOk);
  assert(ca::matchesProtectedShape(header,protected_frame,
                                    sizeof(protected_frame)));
  assert(!ca::matchesProtectedShape(header,protected_frame,
                                     sizeof(protected_frame)-1U));
  header.key_epoch=8U;
  assert(!ca::matchesProtectedShape(header,protected_frame,
                                     sizeof(protected_frame)));
  header.key_epoch=7U;
  protected_frame[4]=0U;  // no custody requested
  assert(!ca::matchesProtectedShape(header,protected_frame,
                                     sizeof(protected_frame)));
  // None of the structural matches is a cryptographic acceptance.
}

void testActualStoreScopeGrantRevokeFloor() {
  Flash flash;
  SecurityStore store(flash,flash);
  provisionV3(flash,store);
  uint8_t wire[ca::kWireSize]{};
  mkWire(wire,kGateA,1U);
  ca::Header ack{};
  assert(ca::decodeHeader(wire,sizeof(wire),ack));
  const auto scoped=[&]() {
    return store.custodyAckScopeMatches(
        ack.tracker_device_id,ack.gateway_device_id,ack.key_epoch,
        ack.custody_floor,ack.grant_generation);
  };

  // v3 but no enrolled Gateway: nothing in untrusted ACK creates a grant.
  assert(!scoped());
  assert(Seam::grant(store,0U,kGateA));
  assert(!scoped()); // staging is NEVER authority
  settle(store);
  assert(!scoped()); // unread durable-result ownership fail-closed
  bool committed=false;
  assert(Seam::take(store,committed)&&committed);
  assert(scoped());
  assert(!store.custodyAckScopeMatches(kDevice,kGateB,7U,0U,1U));
  assert(!store.custodyAckScopeMatches(kDevice+1U,kGateA,7U,0U,1U));
  assert(!store.custodyAckScopeMatches(kDevice,kGateA,8U,0U,1U));
  assert(!store.custodyAckScopeMatches(kDevice,kGateA,7U,1U,1U));
  assert(!store.custodyAckScopeMatches(kDevice,kGateA,7U,0U,2U));

  assert(Seam::revoke(store,0U,kGateA));
  assert(!scoped()); // live pending revoke must also fail closed
  settle(store);
  assert(Seam::take(store,committed)&&committed);
  assert(!scoped());
  assert(Seam::grant(store,2U,kGateA));
  settle(store);
  assert(Seam::take(store,committed)&&committed);
  assert(!scoped()); // stale generation 1 fails after re-grant
  assert(store.custodyAckScopeMatches(kDevice,kGateA,7U,0U,3U));
  assert(Seam::floor(store,5U));
  settle(store);
  assert(Seam::take(store,committed)&&committed);
  assert(!store.custodyAckScopeMatches(kDevice,kGateA,7U,0U,3U));
  assert(store.custodyAckScopeMatches(kDevice,kGateA,7U,5U,3U));

  SecurityStore reboot(flash,flash);
  assert(reboot.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  assert(reboot.custodyAckScopeMatches(kDevice,kGateA,7U,5U,3U)==false);
  // begin() may have scheduled an automatic TX reservation; wait until
  // stable and verify the same durable policy scope remains.
  settle(reboot);
  assert(reboot.custodyAckScopeMatches(kDevice,kGateA,7U,5U,3U));
  assert(!reboot.custodyAckScopeMatches(kDevice,kGateA,7U,0U,3U));
}
} // namespace

int main() {
  testExactHeaderAndObjectShape();
  testActualStoreScopeGrantRevokeFloor();
  puts("SF5K3K real SecurityStore custody ACK scope + wire shape: PASS");
}
