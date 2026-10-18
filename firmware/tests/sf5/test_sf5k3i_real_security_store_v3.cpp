// SF5K3I: the REAL SecurityStore owns v2->v3 upgrade, boot recovery,
// and v3 TX/A2D reservations through the production FlashBackend.
// Gateway ACK/release remains deliberately absent.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <array>
#include "security_store.h"
#include "security_v3_ab_preflight.h"
#include "security_format.h"
#include "sf5k3c_security_v3_candidate.h"
#include "sf5k3d_security_v3_snapshot_candidate.h"

using namespace orun_tlp;
namespace sf = security_format;
namespace ab = security_v3_ab_preflight;
namespace c = sf5k3c_candidate;
namespace d = sf5k3d_candidate;

namespace {
constexpr uint64_t kDevice = UINT64_C(0x0102030405060708);

struct Flash : FlashBackend {
  std::array<uint8_t,8192U> memory{};
  int failure_at_program = -1;
  int cut_bytes = 0;
  uint32_t programs = 0U, erases = 0U;
  Flash() { memory.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t off, void* out, size_t len) const override {
    if (!out || off > memory.size() || len > memory.size() - off)
      return false;
    memcpy(out, memory.data() + off, len);
    return true;
  }
  FlashOpResult program(uint32_t off, const void* data, size_t len) override {
    ++programs;
    if (!data || !len || (off & 3U) || (len & 3U) ||
        off > memory.size() || len > memory.size() - off)
      return FlashOpResult::kFailed;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (size_t i=0;i<len;++i)
      if (memory[off+i] != 0xFFU) return FlashOpResult::kFailed;
    const size_t write_size =
        static_cast<int>(programs) == failure_at_program ?
        static_cast<size_t>(cut_bytes) : len;
    for (size_t i=0;i<len && i<write_size;++i)
      memory[off+i] &= src[i];
    return static_cast<int>(programs) == failure_at_program ?
        FlashOpResult::kFailed : FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t page) override {
    ++erases;
    if (page >= 2U) return FlashOpResult::kFailed;
    memset(memory.data() + page*4096U, 0xFF, 4096U);
    return FlashOpResult::kDone;
  }
};

sf::Credential credential() {
  sf::Credential x{};
  x.device_identity = kDevice;
  x.key_epoch = 7U;
  for (size_t i=0;i<16U;++i)
    x.credential_id[i] = static_cast<uint8_t>(1U+i);
  for (size_t i=0;i<32U;++i)
    x.k_root[i] = static_cast<uint8_t>(0xA0U+i);
  return x;
}

void runUntilIdle(SecurityStore& store) {
  for(unsigned i=0;i<1500U && store.busy();++i) store.poll();
  assert(!store.busy());
}

void provisionV2(Flash& flash, SecurityStore& s) {
  assert(s.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  assert(s.state() == SecurityState::kUnprovisioned);
  sf::Credential c = credential();
  assert(s.commitCredential(c.credential_id, c.key_epoch, c.k_root));
  runUntilIdle(s);
  bool committed = false;
  assert(s.takeCommitResult(committed) && committed);
  assert(s.state() == SecurityState::kProvisioned);
}

void addA2dBound(SecurityStore& s, uint64_t counter) {
  sf::Credential c = credential();
  assert(s.submitAuthenticatedA2dCounter(c.credential_id,
                                         c.key_epoch, counter));
  runUntilIdle(s);
  bool accepted = false;
  assert(s.takeA2dReplayResult(accepted) && accepted);
}

void testMigratedCountersAndReboot() {
  Flash flash;
  SecurityStore s(flash,flash);
  provisionV2(flash,s);
  addA2dBound(s,7U);
  const auto before = flash.memory;
  assert(s.requestV3Migration());
  assert(!s.requestV3Migration()); // no duplicate in-flight migration
  runUntilIdle(s);
  assert(s.state() == SecurityState::kProvisioned);
  assert(memcmp(flash.memory.data(), before.data(),4096U)==0);
  ab::Summary summary{};
  assert(ab::inspect(flash,kDevice,summary)==ab::Result::kV3);
  assert(summary.selected_version==3U);
  assert(summary.tx_reserved_bound==256U);
  assert(summary.a2d_replay_bound==8U);
  assert(summary.active_gateway_count==0U);
  assert(!s.requestV3Migration());
  assert(!s.commitCredential(credential().credential_id,
                             7U,credential().k_root));

  SecurityStore rebooted(flash,flash);
  assert(rebooted.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  assert(rebooted.state()==SecurityState::kProvisioned);
  runUntilIdle(rebooted);
  uint64_t counter = 0U;
  uint32_t epoch = 0U;
  assert(rebooted.reserveNextTxCounter(counter,epoch));
  assert(counter>=256U && epoch==7U);
  // Below the reboot-burned A2D bound must not be admitted again.
  sf::Credential c=credential();
  assert(rebooted.submitAuthenticatedA2dCounter(c.credential_id,7U,7U));
  bool accepted=true;
  assert(rebooted.takeA2dReplayResult(accepted) && !accepted);
  assert(rebooted.submitAuthenticatedA2dCounter(c.credential_id,7U,8U));
  runUntilIdle(rebooted);
  accepted=false;
  assert(rebooted.takeA2dReplayResult(accepted) && accepted);
  assert(ab::inspect(flash,kDevice,summary)==ab::Result::kV3);
  assert(summary.a2d_replay_bound>=16U);
}

void testWriterFailureLocksSameBootAndRecoversV2() {
  Flash flash;
  SecurityStore s(flash,flash);
  provisionV2(flash,s);
  const auto before=flash.memory;
  flash.failure_at_program=static_cast<int>(flash.programs)+1;
  flash.cut_bytes=8;
  assert(s.requestV3Migration());
  runUntilIdle(s);
  assert(s.state()==SecurityState::kFault);
  assert(memcmp(flash.memory.data(),before.data(),4096U)==0);
  flash.failure_at_program=-1;
  SecurityStore restart(flash,flash);
  assert(restart.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  assert(restart.state()==SecurityState::kProvisioned);
  runUntilIdle(restart);
  ab::Summary summary{};
  assert(ab::inspect(flash,kDevice,summary)==ab::Result::kV2);
}

void testV3GatewayTombstonesSurviveLiveCounterAppend() {
  Flash flash;
  const sf::Credential cred=credential();
  c::State seeded{};
  memcpy(seeded.credential_id,cred.credential_id,16U);
  seeded.key_epoch=cred.key_epoch;
  seeded.tx_bound=768U;
  seeded.a2d_bound=24U;
  for(uint8_t slot=0U;slot<2U;++slot){
    c::Entry grant{};
    memcpy(grant.credential_id,seeded.credential_id,16U);
    grant.key_epoch=seeded.key_epoch;
    grant.kind=c::Kind::kCustodyPolicy;
    grant.slot=slot;
    grant.value=UINT64_C(0x1111000011110001)+slot;
    grant.grant_generation=slot+1U;
    grant.policy_revision=slot+1U;
    assert(c::applyHostAuthenticated(grant,true,seeded));
  }
  c::Entry revoked{};
  memcpy(revoked.credential_id,seeded.credential_id,16U);
  revoked.key_epoch=seeded.key_epoch;
  revoked.kind=c::Kind::kCustodyPolicy;
  revoked.slot=0U;
  revoked.value=0U;
  revoked.grant_generation=3U;
  revoked.policy_revision=3U;
  assert(c::applyHostAuthenticated(revoked,true,seeded));
  uint8_t image[c::kPageSize]{};
  assert(d::makeV3SnapshotPage(kDevice,3U,seeded,
                              cred.k_root,image));
  memcpy(flash.memory.data(),image,sizeof(image));

  SecurityStore store(flash,flash);
  assert(store.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  assert(store.state()==SecurityState::kProvisioned);
  runUntilIdle(store);
  ab::Summary summary{};
  assert(ab::inspect(flash,kDevice,summary)==ab::Result::kV3);
  assert(summary.policy_revision==3U &&
         summary.active_gateway_count==1U &&
         summary.max_grant_generation==3U);
  assert(summary.tx_reserved_bound>=1024U);
  addA2dBound(store,24U);
  assert(ab::inspect(flash,kDevice,summary)==ab::Result::kV3);
  assert(summary.a2d_replay_bound>=32U);
  assert(summary.policy_revision==3U &&
         summary.active_gateway_count==1U &&
         summary.max_grant_generation==3U);

  SecurityStore reboot(flash,flash);
  assert(reboot.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  assert(reboot.state()==SecurityState::kProvisioned);
  runUntilIdle(reboot);
  assert(ab::inspect(flash,kDevice,summary)==ab::Result::kV3);
  assert(summary.policy_revision==3U &&
         summary.active_gateway_count==1U &&
         summary.max_grant_generation==3U);
}
}  // namespace

int main(){
  testMigratedCountersAndReboot();
  testWriterFailureLocksSameBootAndRecoversV2();
  testV3GatewayTombstonesSurviveLiveCounterAppend();
  puts("SF5K3I REAL SecurityStore v3 migration/reboot/TX-A2D/tombstones: PASS");
}
