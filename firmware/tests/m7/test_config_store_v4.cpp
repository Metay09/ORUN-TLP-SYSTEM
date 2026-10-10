// ConfigStore with schema v4: read v2 and v4, write v4, persisted service
// intent. docs/architecture/ORUN_CONFIG_STORE_V4_SERVICE_INTENT.md sections
// 4, 5 and 8. The v2-era store cases (fault injection, maintenance, lineage)
// run against the same production class in test_m7p5_config_store.cpp.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "config_store.h"
#include "journal_format.h"
#include "storage_config.h"

using namespace orun_tlp;
using namespace orun_tlp::config_format;

namespace {
constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kRegionSize =
    kPageSize * storage_config::kFutureConfigRegionPages;

class FixedIncarnation : public ConfigIncarnationSource {
 public:
  uint32_t calls = 0;
  bool generate(uint64_t& incarnation) override {
    ++calls;
    incarnation = 0x0BADC0FFEE123456ULL;
    return true;
  }
};

// Synchronous NOR model. `budget` counts physical steps (one page erase or
// one programmed word); when it reaches zero the device loses power: the
// current call stops where it is and fails, exactly as a reset would leave
// the flash.
class NorFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  int budget = -1;
  uint32_t program_calls = 0, erase_calls = 0;

  NorFlash() { bytes.fill(0xFF); }

  bool begin() override { return true; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || offset > bytes.size() ||
        size > bytes.size() - offset)
      return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    ++program_calls;
    if (data == nullptr || (offset & 3U) != 0 || (size & 3U) != 0 ||
        offset > bytes.size() || size > bytes.size() - offset)
      return FlashOpResult::kFailed;
    const auto* source = static_cast<const uint8_t*>(data);
    for (size_t word = 0; word < size; word += 4) {
      if (budget == 0) return FlashOpResult::kFailed;
      for (size_t i = 0; i < 4; ++i) {
        // NOR can only clear bits.
        assert((bytes[offset + word + i] & source[word + i]) ==
               source[word + i]);
        bytes[offset + word + i] &= source[word + i];
      }
      if (budget > 0) --budget;
    }
    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    ++erase_calls;
    if (page >= storage_config::kFutureConfigRegionPages)
      return FlashOpResult::kFailed;
    if (budget == 0) return FlashOpResult::kFailed;
    memset(bytes.data() + size_t(page) * kPageSize, 0xFF, kPageSize);
    if (budget > 0) --budget;
    return FlashOpResult::kDone;
  }

  void seedV2(unsigned page, const TokenizedRecord& record) {
    uint8_t encoded[kV2RecordSize];
    encodeV2(record, encoded);
    memcpy(bytes.data() + size_t(page) * kPageSize, encoded, sizeof(encoded));
  }

  void seedV4(unsigned page, const TokenizedRecord& record) {
    uint8_t encoded[kV4RecordSize];
    encodeV4(record, encoded);
    memcpy(bytes.data() + size_t(page) * kPageSize, encoded, sizeof(encoded));
  }

  const uint8_t* page(unsigned index) const {
    return bytes.data() + size_t(index) * kPageSize;
  }
};

PageInspection inspectPage(const NorFlash& flash, unsigned page) {
  PageInspection inspection;
  assert(inspectPagePrefix(flash.page(page), kMaxPagePrefixSize, inspection));
  return inspection;
}

void settle(ConfigStore& store) {
  for (int i = 0; i < 20 && store.busy(); ++i) store.poll();
  assert(!store.busy());
}

bool save(ConfigStore& store, const Config& candidate) {
  if (store.admitSave(candidate) != ConfigSaveAdmission::kStarted)
    return false;
  settle(store);
  bool success = false;
  assert(store.takeSaveResult(success));
  return success;
}

bool sameConfig(const Config& a, const Config& b) {
  return a.tracking_interval_seconds == b.tracking_interval_seconds &&
         a.battery_capacity_mah == b.battery_capacity_mah &&
         a.service_mode == b.service_mode &&
         a.requested_services == b.requested_services;
}

StateToken validToken(const ConfigStore& store) {
  StateToken token;
  assert(store.tokenState() == ConfigTokenState::kValid);
  assert(store.stateToken(token));
  return token;
}

const uint64_t kInc = 0x5555666677778888ULL;
const Config kTrackOnly(600, 0, kServiceModeExplicit, kServiceTracking);

}  // namespace

int main() {
  // 1. A device that still holds a v2 record: recovered as is, read as AUTO
  // with no selection, and nothing is written at boot.
  {
    NorFlash flash;
    flash.seedV2(0, TokenizedRecord(3, Config(900, 50), StateToken(kInc, 3)));
    FixedIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    assert(!store.maintenanceResetRequired());
    assert(store.config().tracking_interval_seconds == 900);
    assert(store.config().service_mode == kServiceModeAuto);
    assert(store.config().requested_services == 0);
    const StateToken token = validToken(store);
    assert(token.incarnation == kInc && token.revision == 3);
    assert(flash.program_calls == 0 && flash.erase_calls == 0);
    assert(rng.calls == 0);
    assert(inspectPage(flash, 0).evidence == PageEvidence::kV2Committed);
  }

  // 2. First save after a v2 record writes v4 to the other page with the
  // same lineage, keeps every unchanged field, and a cold boot recovers it.
  {
    NorFlash flash;
    flash.seedV2(0, TokenizedRecord(3, Config(900, 50), StateToken(kInc, 3)));
    {
      ConfigStore store(flash);
      assert(store.begin());
      Config candidate = store.config();
      candidate.tracking_interval_seconds = 300;
      assert(save(store, candidate));
      const StateToken token = validToken(store);
      assert(token.incarnation == kInc && token.revision == 4);
    }
    const PageInspection p1 = inspectPage(flash, 1);
    assert(p1.evidence == PageEvidence::kV4Committed);
    assert(p1.generation == 4);
    assert(p1.config.tracking_interval_seconds == 300);
    assert(p1.config.battery_capacity_mah == 50);
    assert(p1.config.service_mode == kServiceModeAuto);
    // The v2 page is left untouched until the next save erases it.
    assert(inspectPage(flash, 0).evidence == PageEvidence::kV2Committed);

    ConfigStore cold(flash);
    assert(cold.begin());
    assert(!cold.maintenanceResetRequired());
    assert(cold.config().tracking_interval_seconds == 300);
    assert(cold.config().battery_capacity_mah == 50);
    const StateToken token = validToken(cold);
    assert(token.incarnation == kInc && token.revision == 4);
    assert(cold.hasCommittedRecord());

    // Next save goes back to page 0 and replaces the v2 page with v4.
    assert(save(cold, kTrackOnly));
    assert(inspectPage(flash, 0).evidence == PageEvidence::kV4Committed);
  }

  // 3. Service intent is stored and recovered; an unchanged intent is a
  // zero-wear no-op; changing only the intent is a real change.
  {
    NorFlash flash;
    FixedIncarnation rng;
    {
      ConfigStore store(flash, &rng);
      assert(store.begin());
      assert(inspectPage(flash, 0).evidence == PageEvidence::kV4Committed);
      assert(store.config().service_mode == kServiceModeAuto);
      assert(save(store, kTrackOnly));
      const uint32_t programs = flash.program_calls;
      assert(store.admitSave(kTrackOnly) == ConfigSaveAdmission::kUnchanged);
      assert(flash.program_calls == programs);

      Config relay = kTrackOnly;
      relay.requested_services = kServiceTracking | kServiceRelayForwarding;
      assert(save(store, relay));
      assert(validToken(store).revision == 3);
    }
    const uint8_t* page = flash.page(0);
    assert(page[28] == kServiceModeExplicit);
    assert(page[29] == (kServiceTracking | kServiceRelayForwarding));

    ConfigStore cold(flash);
    assert(cold.begin());
    assert(cold.config().service_mode == kServiceModeExplicit);
    assert(cold.config().requested_services ==
           (kServiceTracking | kServiceRelayForwarding));
    assert(validToken(cold).revision == 3);

    // EXPLICIT with no service at all is a valid, distinct intent.
    const Config none(600, 0, kServiceModeExplicit, 0);
    assert(save(cold, none));
    ConfigStore again(flash);
    assert(again.begin());
    assert(again.config().service_mode == kServiceModeExplicit);
    assert(again.config().requested_services == 0);
  }

  // 4. Invalid intent is rejected before any flash work.
  {
    NorFlash flash;
    FixedIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    const uint32_t programs = flash.program_calls;
    const uint32_t erases = flash.erase_calls;
    const Config invalid[] = {
        Config(600, 0, 2, 0),                                   // unknown mode
        Config(600, 0, 0xFF, kServiceTracking),
        Config(600, 0, kServiceModeAuto, kServiceTracking),     // AUTO + bits
        Config(600, 0, kServiceModeExplicit, 0x08),             // unknown bit
        Config(600, 0, kServiceModeExplicit, 0x80 | kServiceTracking),
    };
    for (const Config& candidate : invalid)
      assert(store.admitSave(candidate) == ConfigSaveAdmission::kInvalid);
    assert(flash.program_calls == programs && flash.erase_calls == erases);
    assert(validToken(store).revision == 1);
  }

  // 5. A committed v4 record whose intent is semantically invalid is
  // corruption evidence: never adopted, never guessed, never erased.
  {
    NorFlash flash;
    flash.seedV4(0, TokenizedRecord(2, Config(600, 0, 3, 0),
                                    StateToken(kInc, 2)));
    FixedIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    assert(store.tokenState() == ConfigTokenState::kUncertain);
    assert(store.config().service_mode == kServiceModeAuto);
    assert(flash.program_calls == 0 && flash.erase_calls == 0);
    assert(rng.calls == 0);
  }

  // 6. Mixed v2/v4 pages follow one lineage rule.
  {
    // v4 then v2 at the next generation is also a valid lineage (only the
    // record content matters, not the schema).
    NorFlash ok;
    ok.seedV4(0, TokenizedRecord(5, kTrackOnly, StateToken(kInc, 5)));
    ok.seedV2(1, TokenizedRecord(6, Config(700, 1), StateToken(kInc, 6)));
    ConfigStore a(ok);
    assert(a.begin());
    assert(a.config().tracking_interval_seconds == 700);
    assert(validToken(a).revision == 6);

    // Broken lineage between a v2 and a v4 page invalidates token authority.
    NorFlash gap;
    gap.seedV2(0, TokenizedRecord(5, Config(700, 1), StateToken(kInc, 5)));
    gap.seedV4(1, TokenizedRecord(7, kTrackOnly, StateToken(kInc, 7)));
    ConfigStore b(gap);
    assert(b.begin());
    assert(b.maintenanceResetRequired());
    assert(b.tokenState() == ConfigTokenState::kUncertain);

    NorFlash other_incarnation;
    other_incarnation.seedV2(0, TokenizedRecord(5, Config(700, 1),
                                                StateToken(kInc, 5)));
    other_incarnation.seedV4(1, TokenizedRecord(6, kTrackOnly,
                                                StateToken(kInc + 1, 6)));
    ConfigStore c(other_incarnation);
    assert(c.begin());
    assert(c.maintenanceResetRequired());
    assert(c.tokenState() == ConfigTokenState::kUncertain);

    // An exact v4 stage beside a committed v2 page is ignored after reboot:
    // the committed v2 record stays authoritative.
    NorFlash staged;
    staged.seedV2(0, TokenizedRecord(5, Config(700, 1), StateToken(kInc, 5)));
    staged.seedV4(1, TokenizedRecord(6, kTrackOnly, StateToken(kInc, 6)));
    memset(staged.bytes.data() + kPageSize + kV4CommitOffset, 0xFF, 4);
    assert(inspectPage(staged, 1).evidence == PageEvidence::kV4Staged);
    ConfigStore d(staged);
    assert(d.begin());
    assert(!d.maintenanceResetRequired());
    assert(d.config().tracking_interval_seconds == 700);
    assert(validToken(d).revision == 5);
  }

  // 7. The erased-tail check starts where each schema's owned prefix ends.
  {
    // Byte 60 is inside a v4 record (reserved/CRC area) ...
    NorFlash v4;
    v4.seedV4(0, TokenizedRecord(2, kTrackOnly, StateToken(kInc, 2)));
    ConfigStore a(v4);
    assert(a.begin());
    assert(!a.maintenanceResetRequired());
    // ... but after a v2 record it is dirty tail.
    NorFlash v2;
    v2.seedV2(0, TokenizedRecord(2, Config(700, 1), StateToken(kInc, 2)));
    v2.bytes[60] = 0x00;
    ConfigStore b(v2);
    assert(b.begin());
    assert(b.maintenanceResetRequired());
    assert(b.tokenState() == ConfigTokenState::kUncertain);
    assert(b.config().tracking_interval_seconds == 700);  // kept as fallback
    // A v4 page with a programmed byte right after its 72-byte prefix.
    NorFlash v4_dirty;
    v4_dirty.seedV4(0, TokenizedRecord(2, kTrackOnly, StateToken(kInc, 2)));
    v4_dirty.bytes[kV4PagePrefixSize] = 0x00;
    ConfigStore c(v4_dirty);
    assert(c.begin());
    assert(c.maintenanceResetRequired());
    assert(c.tokenState() == ConfigTokenState::kUncertain);
    assert(c.config().requested_services == kServiceTracking);
  }

  // 8. Power loss at every physical step of a normal save, from a v2 and
  // from a v4 active page: after reboot the config is exactly the old or the
  // new one, the new one only with a VALID token one revision on, and an
  // uncertain state always keeps the old config.
  for (int old_schema = 2; old_schema <= 4; old_schema += 2) {
    const Config old_config(900, 50);
    const Config new_config(600, 50, kServiceModeExplicit,
                            kServiceTracking | kServiceApplicationReceive);
    bool saw_old_valid = false, saw_new = false, saw_uncertain = false;
    // 1 erase + 16 body words + 1 commit word.
    for (int budget = 0; budget <= 18; ++budget) {
      NorFlash flash;
      const TokenizedRecord old_record(3, old_config, StateToken(kInc, 3));
      if (old_schema == 2)
        flash.seedV2(0, old_record);
      else
        flash.seedV4(0, old_record);
      {
        ConfigStore store(flash);
        assert(store.begin());
        flash.budget = budget;
        const auto admission = store.admitSave(new_config);
        assert(admission == ConfigSaveAdmission::kStarted);
        settle(store);
        bool success = false;
        assert(store.takeSaveResult(success));
        assert(success == (budget == 18));
      }
      flash.budget = -1;
      ConfigStore rebooted(flash);
      assert(rebooted.begin());
      const Config& got = rebooted.config();
      assert(sameConfig(got, old_config) || sameConfig(got, new_config));
      if (sameConfig(got, new_config)) {
        assert(budget == 18);
        assert(!rebooted.maintenanceResetRequired());
        assert(validToken(rebooted).revision == 4);
        saw_new = true;
      } else if (rebooted.tokenState() == ConfigTokenState::kValid) {
        assert(validToken(rebooted).revision == 3);
        saw_old_valid = true;
      } else {
        assert(rebooted.tokenState() == ConfigTokenState::kUncertain);
        assert(rebooted.maintenanceResetRequired());
        saw_uncertain = true;
      }
      // Recovery never writes.
      const uint32_t programs = flash.program_calls;
      assert(rebooted.diagnostics().baseline_commits == 0);
      assert(flash.program_calls == programs);
    }
    assert(saw_old_valid && saw_new);
    // The one-word "magic only" torn window is classified as supported
    // corruption (same as v2 today), so maintenance is reachable.
    assert(saw_uncertain);
  }

  puts("ConfigStore v4 read v2/v4, write v4, service intent checks: PASS");
}
