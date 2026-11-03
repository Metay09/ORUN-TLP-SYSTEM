// ConfigStore schema v8 (4c-1a, 2026-11-04): v4 layout plus the Bluetooth
// window after each report. Reads v2/v4/v8, writes only v8, keeps lineage.
// docs/architecture/ORUN_CONFIG_STORE_V4_SERVICE_INTENT.md section 12.
#define main config_store_v4_host_main
#include "test_config_store_v4.cpp"
#undef main

namespace {

void testCodec() {
  const TokenizedRecord record(
      7, Config(180, 1200, kServiceModeExplicit, kServiceTracking, 45),
      StateToken(kInc, 9));
  uint8_t v8[kV4RecordSize];
  encodeV8(record, v8);
  // Same layout as v4 except the version byte and the window at 44..45.
  uint8_t v4[kV4RecordSize];
  encodeV4(record, v4);
  assert(v8[4] == kV8Version && v4[4] == kV4Version);
  assert(journal_format::get16(v8 + kV8BleWindowOffset) == 45);
  for (uint32_t i = kV8ReservedOffset; i < kV4CrcOffset; ++i) assert(v8[i] == 0);
  for (uint32_t i = 0; i < kV4CrcOffset; ++i)
    if (i != 4 && i != 44 && i != 45) assert(v8[i] == v4[i]);
  assert(journal_format::get32(v8 + kV4CrcOffset) ==
         journal_format::crc32(v8, kV4CrcOffset));
  assert(journal_format::get32(v8 + kV4CommitOffset) == kCommit);

  TokenizedRecord decoded;
  assert(decodeV4(v8, decoded));
  assert(decoded.config.ble_window_seconds == 45 &&
         decoded.config.tracking_interval_seconds == 180 &&
         decoded.config.requested_services == kServiceTracking);
  // A v4 record reads with the default window.
  assert(decodeV4(v4, decoded));
  assert(decoded.config.ble_window_seconds == kDefaultBleWindowSeconds);
  // 0 (off) is a legal stored value.
  TokenizedRecord off = record;
  off.config.ble_window_seconds = 0;
  encodeV8(off, v8);
  assert(decodeV4(v8, decoded) && decoded.config.ble_window_seconds == 0);
  // The rest of the reserved room must stay zero, including the v2
  // discriminator bytes 48..51.
  for (const uint32_t offset : {46U, 47U, 48U, 51U, 59U}) {
    encodeV8(record, v8);
    v8[offset] = 1;
    journal_format::put32(v8 + kV4CrcOffset, journal_format::crc32(v8, kV4CrcOffset));
    assert(!decodeV4Body(v8, decoded));
  }
  // The classifier knows v8: committed, not "unsupported newer".
  encodeV8(record, v8);
  uint8_t page[kV4PagePrefixSize];
  memset(page, 0xFF, sizeof(page));
  memcpy(page, v8, sizeof(v8));
  PageInspection inspection;
  assert(inspectPagePrefix(page, sizeof(page), inspection));
  assert(inspection.evidence == PageEvidence::kV4Committed);
  assert(inspection.config.ble_window_seconds == 45);
}

void testStore() {
  // A device holding a v4 record: default window, nothing written at boot.
  NorFlash flash;
  flash.seedV4(0, TokenizedRecord(5, kTrackOnly, StateToken(kInc, 5)));
  {
    FixedIncarnation rng;
    ConfigStore store(flash, &rng);
    assert(store.begin());
    assert(store.config().ble_window_seconds == kDefaultBleWindowSeconds);
    assert(flash.program_calls == 0 && flash.erase_calls == 0);
    // Out of range is refused, unchanged is a no-op, only the window is a
    // real change and is written as v8 with the same lineage.
    Config candidate = store.config();
    candidate.ble_window_seconds = kMaxBleWindowSeconds + 1;
    assert(store.admitSave(candidate) == ConfigSaveAdmission::kInvalid);
    assert(store.admitSave(store.config()) == ConfigSaveAdmission::kUnchanged);
    candidate.ble_window_seconds = 60;
    assert(save(store, candidate));
    const StateToken token = validToken(store);
    assert(token.incarnation == kInc && token.revision == 6);
  }
  const PageInspection p1 = inspectPage(flash, 1);
  assert(p1.evidence == PageEvidence::kV4Committed);
  assert(flash.page(1)[4] == kV8Version);
  assert(p1.config.ble_window_seconds == 60 && p1.generation == 6);
  assert(sameConfig(p1.config, kTrackOnly));   // other fields unchanged

  // Cold boot recovers v8 next to the older v4 page; the next save writes
  // v8 over the v4 page; 0 = off survives a reboot.
  ConfigStore cold(flash);
  assert(cold.begin());
  assert(!cold.maintenanceResetRequired());
  assert(cold.config().ble_window_seconds == 60);
  Config off = cold.config();
  off.ble_window_seconds = 0;
  assert(save(cold, off));
  assert(flash.page(0)[4] == kV8Version);
  ConfigStore again(flash);
  assert(again.begin());
  assert(again.config().ble_window_seconds == 0);
  assert(validToken(again).revision == 7);
}

}  // namespace

int main() {
  testCodec();
  testStore();
  puts("ConfigStore v8 read v2/v4/v8, write v8, Bluetooth window checks: PASS");
  return 0;
}
