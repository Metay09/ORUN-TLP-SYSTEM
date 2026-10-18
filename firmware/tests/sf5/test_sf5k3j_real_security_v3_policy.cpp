// SF5K3J REAL SecurityStore v3 Gateway policy durable journal tests.
// Host-only privileged friend: NOT compiled as a production authority.
// There is NO RF/BLE/admin/ACK/record-release invocation in firmware.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <array>
#include "security_store.h"
#include "security_format.h"
#include "security_v3_ab_preflight.h"

using namespace orun_tlp;
namespace sf = orun_tlp::security_format;
namespace ab = orun_tlp::security_v3_ab_preflight;

namespace orun_tlp {
class SecurityV3PolicyTestSeam {
 public:
  static bool grant(SecurityStore& s, uint8_t slot, uint64_t id) {
    return s.beginV3PolicyMutation(SecurityStore::PolicyAction::kGrant,
                                   slot, id, s.v3_snapshot_.custody_floor);
  }
  static bool revoke(SecurityStore& s, uint8_t slot, uint64_t id) {
    return s.beginV3PolicyMutation(SecurityStore::PolicyAction::kRevoke,
                                   slot, id, s.v3_snapshot_.custody_floor);
  }
  static bool floor(SecurityStore& s, uint32_t next) {
    return s.beginV3PolicyMutation(
        SecurityStore::PolicyAction::kAdvanceFloor, 0xFFU, 0U, next);
  }
  static bool take(SecurityStore& s, bool& committed) {
    return s.takeV3PolicyResult(committed);
  }
  static uint32_t revision(const SecurityStore& s) {
    return s.v3_snapshot_.policy_revision;
  }
  static uint32_t used(const SecurityStore& s) {
    if (s.active_page_ < 0) return 0U;
    return s.pages_[s.active_page_].state_used;
  }
  static uint32_t generation(const SecurityStore& s, uint8_t slot) {
    return s.v3_snapshot_.gateways[slot].generation;
  }
  static uint64_t id(const SecurityStore& s, uint8_t slot) {
    return s.v3_snapshot_.gateways[slot].gateway_id;
  }
};
}

namespace {
constexpr uint64_t kDevice = UINT64_C(0x0102030405060708);
constexpr uint64_t kGateA = UINT64_C(0x2300230023002301);
constexpr uint64_t kGateB = UINT64_C(0x2300230023002302);
using Seam = orun_tlp::SecurityV3PolicyTestSeam;

struct Flash : public FlashBackend {
  std::array<uint8_t, 8192U> bytes{};
  int fail_program_at = -1;
  unsigned failure_prefix = 0U;
  unsigned programs = 0U;
  unsigned erases = 0U;
  Flash() { bytes.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t offset, void* out, size_t length) const override {
    if (!out || offset > bytes.size() ||
        length > bytes.size() - offset) return false;
    memcpy(out, bytes.data() + offset, length);
    return true;
  }
  FlashOpResult program(uint32_t offset, const void* data,
                        size_t length) override {
    ++programs;
    if (!data || !length || (offset % 4U) || (length % 4U) ||
        offset > bytes.size() || length > bytes.size() - offset)
      return FlashOpResult::kFailed;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0U; i < length; ++i)
      if (bytes[offset+i] != 0xFFU) return FlashOpResult::kFailed;
    const bool fail = static_cast<int>(programs) == fail_program_at;
    const size_t count = fail ?
        (failure_prefix < length ? failure_prefix : length) : length;
    for (size_t i = 0U; i < count; ++i)
      bytes[offset+i] &= src[i];
    return fail ? FlashOpResult::kFailed : FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t page) override {
    ++erases;
    if (page >= 2U) return FlashOpResult::kFailed;
    memset(bytes.data() + page*4096U, 0xFFU, 4096U);
    return FlashOpResult::kDone;
  }
};

sf::Credential credential() {
  sf::Credential c{};
  c.device_identity = kDevice;
  c.key_epoch = 7U;
  for (unsigned i=0U; i<sf::kCredentialIdSize; ++i)
    c.credential_id[i] = static_cast<uint8_t>(3U + i);
  for (unsigned i=0U; i<sf::kKRootSize; ++i)
    c.k_root[i] = static_cast<uint8_t>(0x90U + i);
  return c;
}

void settle(SecurityStore& s, unsigned ceiling=3000U) {
  for (unsigned pass=0U; pass<ceiling && s.busy(); ++pass) s.poll();
  assert(!s.busy());
}

void provisionV3(Flash& flash, SecurityStore& s) {
  (void)flash;
  assert(s.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  sf::Credential c=credential();
  assert(s.commitCredential(c.credential_id, c.key_epoch, c.k_root));
  settle(s);
  bool success=false;
  assert(s.takeCommitResult(success) && success);
  assert(s.requestV3Migration());
  settle(s);
  assert(s.state()==SecurityState::kProvisioned);
  ab::Summary summary{};
  assert(ab::inspect(flash,kDevice,summary)==ab::Result::kV3);
  assert(summary.policy_revision==0U &&
         summary.active_gateway_count==0U);
}

void inspect(Flash& f, uint32_t revision, uint32_t max_generation,
             uint8_t active, uint32_t floor) {
  ab::Summary s{};
  assert(ab::inspect(f,kDevice,s)==ab::Result::kV3);
  assert(s.policy_revision==revision);
  assert(s.max_grant_generation==max_generation);
  assert(s.active_gateway_count==active);
  assert(s.custody_floor==floor);
}

void testDurableGrantRevokeFloorAndReboot() {
  Flash flash;
  SecurityStore s(flash,flash);
  provisionV3(flash,s);
  assert(!Seam::grant(s,4U,kGateA)); // only four slots
  assert(!Seam::grant(s,0U,0U));
  assert(!Seam::revoke(s,0U,kGateA));
  assert(!Seam::floor(s,0U));
  assert(Seam::grant(s,0U,kGateA));
  assert(!Seam::grant(s,1U,kGateB)); // no concurrent mutation
  assert(Seam::revision(s)==0U); // NOT authorized before durable commit
  bool applied=false;
  assert(!Seam::take(s,applied)); // queued is NOT durable
  inspect(flash,0U,0U,0U,0U);
  settle(s);
  assert(Seam::take(s,applied) && applied);
  assert(!Seam::take(s,applied)); // one-shot outcome
  assert(s.state()==SecurityState::kProvisioned);
  assert(Seam::revision(s)==1U);
  assert(Seam::id(s,0U)==kGateA);
  assert(Seam::generation(s,0U)==1U);
  inspect(flash,1U,1U,1U,0U);

  assert(!Seam::grant(s,0U,kGateB)); // replacement needs revoke
  assert(!Seam::grant(s,1U,kGateA)); // duplicate active ID
  assert(!Seam::revoke(s,0U,kGateB)); // exact ID/slot required
  assert(Seam::revoke(s,0U,kGateA));
  assert(Seam::id(s,0U)==kGateA); // old snapshot until commit
  assert(!Seam::take(s,applied));
  settle(s);
  assert(Seam::take(s,applied) && applied);
  assert(Seam::id(s,0U)==0U);
  assert(Seam::generation(s,0U)==2U); // tombstone retained
  inspect(flash,2U,2U,0U,0U);
  assert(!Seam::revoke(s,0U,kGateA));

  assert(Seam::grant(s,1U,kGateA));
  settle(s);
  assert(Seam::take(s,applied) && applied);
  assert(Seam::generation(s,1U)==3U);
  assert(Seam::generation(s,0U)==2U);
  inspect(flash,3U,3U,1U,0U);
  assert(Seam::floor(s,9U));
  assert(!Seam::floor(s,8U)); // in-flight refusal
  settle(s);
  assert(Seam::take(s,applied) && applied);
  assert(!Seam::floor(s,9U)); // global floor must advance
  inspect(flash,4U,3U,1U,9U);
  assert(s.diagnostics().v3_policy_commits==4U);
  SecurityStore reboot(flash,flash);
  assert(reboot.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  assert(reboot.state()==SecurityState::kProvisioned);
  assert(Seam::revision(reboot)==4U &&
         Seam::generation(reboot,0U)==2U &&
         Seam::id(reboot,0U)==0U &&
         Seam::id(reboot,1U)==kGateA &&
         Seam::generation(reboot,1U)==3U);
  assert(!Seam::floor(reboot,9U));
  settle(reboot);
  inspect(flash,4U,3U,1U,9U);
}

void testTornAndAmbiguousPolicyCommit() {
  for (int phase=0;phase<2;++phase) {
    Flash flash;
    SecurityStore s(flash,flash);
    provisionV3(flash,s);
    assert(Seam::grant(s,0U,kGateA));
    flash.fail_program_at=static_cast<int>(flash.programs)+
                           (phase==0 ? 1 : 2);
    flash.failure_prefix=phase==0 ? 8U : 4U;
    s.poll();
    assert(s.state()==SecurityState::kFault);
    assert(!s.busy());
    assert(s.diagnostics().v3_policy_failures==1U);
    bool applied=true;
    assert(Seam::take(s,applied) && !applied);
    assert(Seam::revision(s)==0U && Seam::id(s,0U)==0U);
    const unsigned no_more_writes=flash.programs;
    for(unsigned i=0;i<16U;++i) s.poll();
    assert(flash.programs==no_more_writes);
    flash.fail_program_at=-1;

    SecurityStore reboot(flash,flash);
    assert(reboot.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
    assert(reboot.state()==SecurityState::kProvisioned);
    // Body-only cut never authorized the grant. The commit cut may
    // physically land and is then legitimately recovered on reboot.
    assert(Seam::revision(reboot)==(phase==0 ? 0U : 1U));
    assert(Seam::id(reboot,0U)==(phase==0 ? 0U : kGateA));
  }
}

void testPolicyTriggersV3PageCompaction() {
  Flash flash;
  SecurityStore s(flash,flash);
  provisionV3(flash,s);
  bool applied=false;
  assert(Seam::grant(s,0U,kGateA));
  settle(s);
  assert(Seam::take(s,applied) && applied);
  assert(Seam::revoke(s,0U,kGateA));
  settle(s);
  assert(Seam::take(s,applied) && applied);
  const uint32_t gen=Seam::generation(s,0U);
  assert(gen==2U);

  // Use real TX counter blocks to fill the actual 71-record v3 journal.
  // Stop at 70 occupied entries, then policy update must compact A/B.
  unsigned safety=0U;
  while (Seam::used(s)<70U) {
    if (s.busy()) {
      settle(s);
    } else {
      uint64_t counter=0U;
      uint32_t epoch=0U;
      if (!s.reserveNextTxCounter(counter,epoch)) s.poll();
    }
    assert(++safety<22000U);
    assert(s.state()==SecurityState::kProvisioned);
  }
  const uint32_t comp_before=s.diagnostics().compactions;
  assert(Seam::floor(s,5U));
  assert(s.busy());
  settle(s);
  assert(Seam::take(s,applied) && applied);
  assert(s.state()==SecurityState::kProvisioned);
  assert(s.diagnostics().compactions==comp_before+1U);
  assert(Seam::revision(s)==3U &&
         Seam::generation(s,0U)==2U &&
         Seam::id(s,0U)==0U);
  inspect(flash,3U,2U,0U,5U);
  SecurityStore reboot(flash,flash);
  assert(reboot.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  assert(reboot.state()==SecurityState::kProvisioned);
  assert(Seam::revision(reboot)==3U &&
         Seam::generation(reboot,0U)==2U);
}

} // namespace

int main() {
  testDurableGrantRevokeFloorAndReboot();
  testTornAndAmbiguousPolicyCommit();
  testPolicyTriggersV3PageCompaction();
  puts("SF5K3J REAL SecurityStore v3 Gateway policy journal/revoke/cuts: PASS");
  // SF5K3K includes this fixture with main renamed to an ordinary function.
  // Return explicitly so that both standalone and included builds pass -Werror.
  return 0;
}
