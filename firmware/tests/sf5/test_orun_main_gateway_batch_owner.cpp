// ORUN main-based Gateway 0x0A durable readback: reuse the EXISTING
// GatewayLocalCustodyOwner / AuthorizedIngress / ProductCustodyStore.
// Fake NOR + test OpenSSL; never RF, provisioning, or physical Flash.
#include "kq05_gateway_batch_custody_crypto.h"

#define main original_sf5k3s_fixture_main
#include "test_sf5k3s_gateway_authorized_ingress.cpp"
#undef main

namespace orun_tlp {
// Read-only test peer for real full-digest/HMAC verification.
struct Kq05BatchCryptoHostPeer {
  static bool verify(const uint8_t* wire, size_t size,
                     const GatewayCustodyAckScope& scope,
                     const uint8_t (&key)[32],
                     const Kq05BatchExactView* objects, uint8_t count) {
    return Kq05GatewayBatchCustodyCrypto::verifyAgainstExactRetained(
        wire, size, scope, key, objects, count);
  }
};
}  // namespace orun_tlp

namespace {
namespace batch = orun_tlp::kq05_gateway_batch_ack;
using orun_tlp::Kq05BatchExactView;
using orun_tlp::Kq05BatchCryptoHostPeer;
using Handle = ProductCustodyStore::Handle;
using Ingress = GatewayCustodyAuthorizedIngress;

void durableSingle(Ingress& ingress,
                   GatewayLocalCustodyOwner& owner,
                   ProductCustodyStore& store,
                   uint8_t* object, size_t size,
                   Handle& handle) {

  assert(ingress.receive(object, size, true) ==
         Ingress::Result::kStarted);

  for (unsigned n = 0U;
       n < 150U && !ingress.batchReady(); ++n)
    ingress.service();

  assert(!owner.waitingFlash());

  GatewayCustodyAckScope scope{};

  assert(ingress.takeAuthorizedBatchHandle(handle, scope));
  assert(!ingress.pending());

  Handle duplicate{};

  assert(store.requestCustody(object, size, &duplicate) ==
         ProductCustodyStore::Admission::kDuplicateHeld);

  assert(duplicate.page == handle.page);
  assert(duplicate.slot == handle.slot);
  assert(duplicate.generation == handle.generation);
}

void preservesOutputOnFailure(Ingress& ingress, const Handle* handles,
                              uint8_t count, const GatewayCustodyAckScope& s) {
  uint8_t out[batch::kMaxSize]{};
  memset(out, 0xAB, sizeof(out));
  size_t length = 999U;
  assert(!ingress.prepareAuthorizedBatchAck(handles, count, s, out, length));
  assert(length == 999U);
  for (uint8_t b : out) assert(b == 0xAB);
}

void validatesAllFiveWidths(bool async) {
  for (uint8_t count = 1U; count <= batch::kMaxObjects; ++count) {
    Nor flash; flash.async = async;
    ProductCustodyStore store(flash, 2U);
    assert(store.begin());
    GatewayLocalCustodyOwner owner(store);
    Grants grants;
    Ingress ingress(owner, grants, kGateway);
    uint8_t bytes[batch::kMaxObjects][100]{};
    size_t sizes[batch::kMaxObjects]{};
    Handle handles[batch::kMaxObjects]{};
    Kq05BatchExactView exact[batch::kMaxObjects]{};

    for (uint8_t n = 0U; n < count; ++n) {
      sizes[n] = n % 2U == 0U ? 100U : 84U;
      frame(bytes[n], sizes[n]);
      put64(bytes[n] + 20U, 42U + n);  // unique protected counter
      durableSingle(ingress, owner, store, bytes[n], sizes[n], handles[n]);
      exact[n].bytes = bytes[n];
      exact[n].size = sizes[n];
    }

    GatewayCustodyAckScope trusted{};
    uint8_t key[32]{};
    assert(grants.activeGrant(kTracker, 7U, trusted, key));
    uint8_t signed_ack[batch::kMaxSize]{};
    size_t length = 0U;
    const int flash_writes = flash.writes;
    assert(ingress.prepareAuthorizedBatchAck(
        handles, count, trusted, signed_ack, length));
    assert(length == batch::kHeaderSize + batch::kRefSize * count +
                         batch::kTagSize);
    batch::Header h{};
    assert(batch::decode(signed_ack, length, h));
    assert(h.count == count);
    assert(h.tracker_device_id == kTracker &&
           h.gateway_device_id == kGateway &&
           h.grant_generation == trusted.grant_generation);
    assert(Kq05BatchCryptoHostPeer::verify(
        signed_ack, length, trusted, key, exact, count));
    assert(flash.writes == flash_writes);  // no second storage path

    uint8_t tampered[batch::kMaxSize]{};
    memcpy(tampered, signed_ack, length);
    tampered[length - 1U] ^= 0x80U;
    assert(!Kq05BatchCryptoHostPeer::verify(
        tampered, length, trusted, key, exact, count));

    // Scope change, grant withdrawal, missing handle or duplicate handle
    // may not produce a false custody receipt or modify caller output.
    GatewayCustodyAckScope wrong = trusted;
    ++wrong.grant_generation;
    preservesOutputOnFailure(ingress, handles, count, wrong);
    grants.enrolled = false;
    preservesOutputOnFailure(ingress, handles, count, trusted);
    grants.enrolled = true;
    Handle unknown[batch::kMaxObjects]{};
    memcpy(unknown, handles, sizeof(unknown));
    unknown[0].slot = UINT16_MAX;
    preservesOutputOnFailure(ingress, unknown, count, trusted);
    if (count >= 2U) {
      Handle duplicate[batch::kMaxObjects]{};
      memcpy(duplicate, handles, sizeof(duplicate));
      duplicate[1] = duplicate[0];
      preservesOutputOnFailure(ingress, duplicate, count, trusted);
    }

    // An incomplete single-ACK Flash transaction blocks the batch signer.
    if (count == 1U) {
      uint8_t extra[100]{};
      frame(extra, sizeof(extra));
      put64(extra + 20U, 1234U);
      assert(ingress.receive(extra, sizeof(extra), true) ==
             Ingress::Result::kStarted);
      preservesOutputOnFailure(ingress, handles, count, trusted);
    }
  }
}

void batchAfterInterruptedAppend() {
  Nor flash;
  Grants grants;
  uint8_t object[100]{};
  frame(object, sizeof(object));
  Handle good{};
  {
    ProductCustodyStore store(flash, 2U);
    assert(store.begin());
    GatewayLocalCustodyOwner owner(store);
    Ingress ingress(owner, grants, kGateway);
    durableSingle(ingress, owner, store, object, sizeof(object), good);
    uint8_t next[100];
    memcpy(next, object, sizeof(next));
    put64(next + 20U, 12345U);
    assert(store.requestCustody(next, sizeof(next)) == ProductCustodyStore::Admission::kStarted);
    store.poll();  // body programmed, commit word still erased
  }
  ProductCustodyStore store(flash, 2U);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  Ingress ingress(owner, grants, kGateway);
  GatewayCustodyAckScope scope{};
  uint8_t key[32]{}, wire[batch::kMaxSize]{};
  size_t length = 0;
  assert(grants.activeGrant(kTracker, 7U, scope, key));
  assert(ingress.prepareAuthorizedBatchAck(&good, 1, scope, wire, length));
  Kq05BatchExactView view{};
  view.bytes = object;
  view.size = sizeof(object);
  assert(Kq05BatchCryptoHostPeer::verify(wire, length, scope, key, &view, 1));
  Handle mixed[2] = {good, good};
  ++mixed[1].slot;
  preservesOutputOnFailure(ingress, mixed, 2, scope);
  mixed[1] = good;
  ++mixed[1].generation;
  preservesOutputOnFailure(ingress, mixed, 2, scope);
  grants.enrolled = false;
  preservesOutputOnFailure(ingress, &good, 1, scope);
  puts("ORUN-103 recovered batch: exact signature, torn/stale member and revoked grant rejected PASS");
}

struct RotatingGrants final : GatewayCustodyGrantProvider {
  Grants backing{};
  unsigned calls = 0U;
  bool activeGrant(uint64_t tracker, uint32_t epoch,
                   GatewayCustodyAckScope& scope,
                   uint8_t (&key)[32]) override {
    ++calls;
    if (calls == 2U) ++backing.generation;
    return backing.activeGrant(tracker, epoch, scope, key);
  }
};

void revocationBetweenReadbackAndPublishFails() {
  Nor flash;
  ProductCustodyStore store(flash, 2U);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  Grants stable;
  Ingress first(owner, stable, kGateway);
  uint8_t packet[100]{};
  frame(packet, sizeof(packet));
  Handle handle{};
  durableSingle(first, owner, store, packet, sizeof(packet), handle);
  GatewayCustodyAckScope initial{};
  uint8_t key[32]{};
  assert(stable.activeGrant(kTracker, 7U, initial, key));

  RotatingGrants changes;
  Ingress second(owner, changes, kGateway);
  preservesOutputOnFailure(second, &handle, 1U, initial);
  assert(changes.calls == 2U); // changed *after* exact durable readback
}
}  // namespace


void batchReadbackHandleIsSingleOwner(bool async) {
  // Verify the single durable owner with the current 0x0A protocol.
  Nor nor; nor.async=async;
  ProductCustodyStore store(nor, 2U);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  Grants grants;
  GatewayCustodyAuthorizedIngress ingress(owner,grants,kGateway);
  uint8_t packet[100]{};
  frame(packet,sizeof(packet));
  ProductCustodyStore::Handle held{};
  GatewayCustodyAckScope signed_scope{};
  assert(ingress.receive(packet,sizeof(packet),true)==
         GatewayCustodyAuthorizedIngress::Result::kStarted);
  assert(!owner.batchReady() && !ingress.batchReady());
  for(unsigned j=0U;j<100U && !ingress.batchReady();++j)
    ingress.service();
  assert(ingress.batchReady() && owner.batchReady());
  assert(ingress.takeAuthorizedBatchHandle(held,signed_scope));
  assert(!ingress.pending());
  assert(signed_scope.tracker_device_id==kTracker);
  uint8_t exact[100]{};
  size_t size=0U;
  assert(store.readConfirmedDurable(held,exact,size)==
         ProductCustodyStore::Lookup::kFound);
  assert(size==sizeof(packet) && memcmp(packet,exact,size)==0);
  uint8_t signed_ack[batch::kMaxSize]{};
  size_t written=0U;
  assert(ingress.prepareAuthorizedBatchAck(
      &held,1U,signed_scope,signed_ack,written));
  assert(written==batch::kHeaderSize+batch::kRefSize+batch::kTagSize);
  assert(!ingress.batchReady());

  // Duplicate exact ciphertext consumes zero extra Flash space and
  // re-offers an identical verified handle without another 0x05 ACK.
  const int before=nor.writes;
  assert(ingress.receive(packet,sizeof(packet),true)==
         GatewayCustodyAuthorizedIngress::Result::kAckReady);
  ProductCustodyStore::Handle again{};
  GatewayCustodyAckScope repeat{};
  assert(ingress.takeAuthorizedBatchHandle(again,repeat));
  assert(again.page==held.page && again.slot==held.slot &&
         again.generation==held.generation);
  assert(nor.writes==before);
}

void batchRevokedHandleNeverEscapes() {
  Nor nor;
  ProductCustodyStore store(nor,2U);
  assert(store.begin());
  GatewayLocalCustodyOwner owner(store);
  Grants grants;
  GatewayCustodyAuthorizedIngress ingress(owner,grants,kGateway);
  uint8_t packet[100]{};frame(packet,sizeof(packet));
  assert(ingress.receive(packet,sizeof(packet),true)==
         GatewayCustodyAuthorizedIngress::Result::kStarted);
  for(unsigned j=0U;j<100U && !ingress.batchReady();++j)
    ingress.service();
  assert(ingress.batchReady());
  grants.generation=2U; // revocation AFTER Flash commit but before handoff
  ProductCustodyStore::Handle handle{};
  handle.page=123U;
  GatewayCustodyAckScope scope{};
  scope.grant_generation=999U;
  assert(!ingress.takeAuthorizedBatchHandle(handle,scope));
  assert(handle.page==123U && scope.grant_generation==999U);
  assert(!ingress.pending() && !owner.batchReady());
}

int main() {
  original_sf5k3s_fixture_main();
  batchReadbackHandleIsSingleOwner(false);
  batchReadbackHandleIsSingleOwner(true);
  batchRevokedHandleNeverEscapes();
  batchAfterInterruptedAppend();
  validatesAllFiveWidths(false);
  validatesAllFiveWidths(true);
  revocationBetweenReadbackAndPublishFails();
  puts("ORUN main Gateway single-owner durable 0x0A 1..5 HMAC: PASS (PC fake NOR)");
  puts("NO REAL SX126x / NO BOARD FLASH / NO PRODUCT RUNTIME SWITCH");
  return 0;
}
