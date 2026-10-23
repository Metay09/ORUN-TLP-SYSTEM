// SF5K3X: real ProductCustodyStore boot/mount decisions on fake NOR.
// NEVER provisions credentials, erases flash or activates a Gateway role.
#include <assert.h>
#include <array>
#include <stdio.h>
#include <string.h>

#include "gateway_product_startup_guard.h"
#include "custody_store_product_format.h"

using namespace orun_tlp;

namespace {
constexpr uint16_t kPages = 4U;
constexpr uint64_t kGateway = UINT64_C(0x2300230023002301);

struct Nor final : FlashBackend {
  std::array<uint8_t, 4096U * kPages> bytes{};
  unsigned programs = 0U, erases = 0U, begins = 0U;
  bool fail_read = false, fail_begin = false, unreconciled = false;

  Nor() { bytes.fill(0xFFU); }
  bool begin() override {
    ++begins;
    return !fail_begin;
  }
  bool read(uint32_t offset, void* dest, size_t count) const override {
    if (fail_read || dest == nullptr || offset > bytes.size() ||
        count > bytes.size() - offset) return false;
    memcpy(dest, bytes.data() + offset, count);
    return true;
  }
  bool hasUnreconciledMutation() const override { return unreconciled; }
  FlashOpResult program(uint32_t offset, const void* source,
                        size_t count) override {
    if (!source || count == 0U || (offset % 4U) != 0U ||
        (count % 4U) != 0U || count > 144U ||
        offset > bytes.size() || count > bytes.size() - offset)
      return FlashOpResult::kFailed;
    const uint8_t* src = static_cast<const uint8_t*>(source);
    for (size_t i = 0; i < count; ++i)
      if (bytes[offset+i] != 0xFFU) return FlashOpResult::kFailed;
    ++programs;
    for (size_t i = 0; i < count; ++i) bytes[offset+i] &= src[i];
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t) override {
    ++erases;
    return FlashOpResult::kFailed;  // no erase permitted, even by test
  }
};

struct Enrollment final : GatewayDurableGrantSource {
  bool provisioned = false;
  bool verifiedForLocalGateway(uint64_t id) const override {
    return provisioned && id == kGateway;
  }
  bool activeGrant(uint64_t, uint32_t,
                   GatewayCustodyAckScope&, uint8_t (&key)[32]) override {
    memset(key, 0, sizeof(key));
    return false;  // Startup proof != actual per-Tracker ACK authorization
  }
};

using Gate = GatewayProductStartupGuard;
void refusalScenarios() {
  for (unsigned scenario = 0; scenario < 9U; ++scenario) {
    Nor flash;
    Enrollment grant;
    grant.provisioned = true;
    ProductCustodyStore store(flash, kPages);
    Gate gate(flash, kPages, store, grant, kGateway);
    Gate::State expected = Gate::State::kNotChecked;
    bool service = true, tracker_mounted = false;
    uint64_t hardware = kGateway;
    switch (scenario) {
      case 0U: service = false;
               expected = Gate::State::kServiceDisabled; break;
      case 1U: hardware = kGateway + 1U;
               expected = Gate::State::kInvalidIdentity; break;
      case 2U: tracker_mounted = true;
               expected = Gate::State::kTrackerOwnsRegion; break;
      case 3U: grant.provisioned = false;
               expected = Gate::State::kUnprovisioned; break;
      case 4U: flash.fail_read = true;
               expected = Gate::State::kFlashUnavailable; break;
      case 5U: flash.unreconciled = true;
               expected = Gate::State::kFlashUnavailable; break;
      case 6U: memcpy(flash.bytes.data(), "OBS1", 4U);
               expected = Gate::State::kForeignRegion; break;
      case 7U: memcpy(flash.bytes.data(), "OCQ1", 4U);
               expected = Gate::State::kForeignRegion; break;
      case 8U: memcpy(flash.bytes.data(), "BAD!", 4U);
               expected = Gate::State::kNeedsPreparation; break;
    }
    assert(gate.inspectAndMount(service, tracker_mounted, hardware) ==
           expected);
    assert(!gate.mayServe(true, false, kGateway));
    assert(!store.ready());
    assert(gate.inspectAndMount(true, false, kGateway) == expected);
    assert(flash.programs == 0U && flash.erases == 0U);
  }
}

void safeMountAndDynamicRevocation() {
  Nor flash;
  Enrollment grant;
  grant.provisioned = true;
  ProductCustodyStore store(flash, kPages);
  Gate gate(flash, kPages, store, grant, kGateway);
  assert(gate.inspectAndMount(true, false, kGateway) ==
         Gate::State::kReady);
  assert(store.ready() && gate.mayServe(true, false, kGateway));
  assert(flash.programs == 0U && flash.erases == 0U);
  assert(!gate.mayServe(false, false, kGateway));
  assert(!gate.mayServe(true, true, kGateway));
  assert(!gate.mayServe(true, false, kGateway + 1U));
  grant.provisioned = false;
  assert(!gate.mayServe(true, false, kGateway));
  grant.provisioned = true;
  assert(gate.mayServe(true, false, kGateway));
  flash.unreconciled = true;
  assert(!gate.mayServe(true, false, kGateway));
}

void corruptOCQ2NoErase() {
  Nor flash;
  // An OCQ2-looking torn/corrupt page is NOT permission to recover by erase.
  memcpy(flash.bytes.data(), "OCQ2", 4U);
  Enrollment grant;
  grant.provisioned = true;
  ProductCustodyStore store(flash, kPages);
  Gate gate(flash, kPages, store, grant, kGateway);
  assert(gate.inspectAndMount(true, false, kGateway) ==
         Gate::State::kMountRejected);
  assert(!gate.mayServe(true, false, kGateway));
  assert(flash.programs == 0U && flash.erases == 0U);
}

void mountPersistedHeldObject() {
  Nor flash;
  ProductCustodyStore writer(flash, kPages);
  assert(writer.begin());
  uint8_t object[100]{};
  object[0] = 2U; object[1] = 4U; object[2] = 2U;
  object[3] = 1U; object[4] = 3U; object[5] = 1U; object[6] = 64U;
  // Exact ciphertext is opaque to Gateway: structurally valid only.
  object[8] = 1U; object[19] = 1U; object[27] = 42U;
  assert(writer.requestCustody(object, sizeof(object)) ==
         ProductCustodyStore::Admission::kStarted);
  bool committed = false, received = false;
  ProductCustodyStore::Handle handle{};
  for (unsigned i = 0; i < 100U && !received; ++i) {
    writer.poll();
    received = writer.takeCustodyResult(committed, handle);
  }
  assert(received && committed);
  const unsigned writes = flash.programs;
  Enrollment grant;
  grant.provisioned = true;
  ProductCustodyStore restored(flash, kPages);
  Gate gate(flash, kPages, restored, grant, kGateway);
  assert(gate.inspectAndMount(true, false, kGateway) ==
         Gate::State::kReady);
  assert(gate.mayServe(true, false, kGateway));
  uint8_t found[100]{};
  size_t size = 0U;
  assert(restored.readConfirmedHeld(handle, found, size) ==
         ProductCustodyStore::Lookup::kFound);
  assert(size == sizeof(object) &&
         memcmp(found, object, sizeof(object)) == 0);
  assert(flash.programs == writes && flash.erases == 0U);
}
}  // namespace

int main() {
  refusalScenarios();
  safeMountAndDynamicRevocation();
  corruptOCQ2NoErase();
  mountPersistedHeldObject();
  puts("SF5K3X Gateway boot gate/OCQ2 ownership/revocation: PASS (host)");
  return 0;
}
