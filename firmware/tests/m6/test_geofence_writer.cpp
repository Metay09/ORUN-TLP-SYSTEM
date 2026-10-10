// Local geofence writer: degree parsing, RAM staging and the mutation owner
// against the real GeofenceStore (geofence_staging.h, geofence_mutation.h).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <initializer_list>

#include "geofence_mutation.h"
#include "geofence_staging.h"
#include "storage_config.h"

using namespace orun_tlp;

namespace {

constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kRegionSize =
    kPageSize * storage_config::kGeofenceRegionPages;

class FakeFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  uint32_t program_calls = 0, erase_calls = 0;
  int fail_program_at = -1;
  FakeFlash() { bytes.fill(0xFF); }
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
    if (static_cast<int>(program_calls) == fail_program_at)
      return FlashOpResult::kFailed;
    const auto* source = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) bytes[offset + i] &= source[i];
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t page) override {
    ++erase_calls;
    memset(bytes.data() + size_t(page) * kPageSize, 0xFF, kPageSize);
    return FlashOpResult::kDone;
  }
  bool hasUnreconciledMutation() const override { return false; }
};

class FixedIncarnation : public GeofenceIncarnationSource {
 public:
  bool generate(uint64_t& incarnation) override {
    incarnation = 0x0102030405060708ULL;
    return true;
  }
};

bool degrees(const char* text, int32_t max, int32_t* out) {
  return parseDegreesE7(text, strlen(text), max, out);
}

void parsing() {
  int32_t v = 0;
  assert(degrees("37.8512345", 90, &v) && v == 378512345);
  assert(degrees("-8.5", 90, &v) && v == -85000000);
  assert(degrees("28", 180, &v) && v == 280000000);
  assert(degrees("0.0000001", 90, &v) && v == 1);
  assert(degrees("-0", 90, &v) && v == 0);
  assert(degrees("90", 90, &v) && v == 900000000);
  assert(degrees("-180.0000000", 180, &v) && v == -1800000000);
  v = 77;
  for (const char* bad : {"", "-", ".5", "5.", "1e5", "+5", " 5", "5 ", "1234",
                          "1.12345678", "90.0000001", "--5", "5..1", "a"}) {
    assert(!degrees(bad, 90, &v));
    assert(v == 77);
  }
  assert(!degrees("180.0000001", 180, &v));
  assert(!parseDegreesE7(nullptr, 1, 90, &v));
  assert(!parseDegreesE7("1", 1, 90, nullptr));
}

void staging() {
  GeofenceStaging s;
  assert(!s.active());
  assert(s.addVertex(GeoPointE7(1, 1)) == GeofenceStaging::Result::kNotStaging);
  assert(s.nextArea() == GeofenceStaging::Result::kNotStaging);
  s.begin();
  assert(s.active() && s.areaCount() == 0 && s.vertexCount() == 0);
  assert(s.nextArea() == GeofenceStaging::Result::kEmptyArea);
  for (int i = 0; i < 4; ++i)
    assert(s.addVertex(GeoPointE7(i, i)) == GeofenceStaging::Result::kOk);
  assert(s.areaCount() == 1 && s.vertexCount() == 4);
  assert(s.nextArea() == GeofenceStaging::Result::kOk);
  assert(s.areaCount() == 1);  // the new area is still empty
  for (int i = 0; i < 3; ++i) s.addVertex(GeoPointE7(100 + i, 100 + i));
  GeofencePolygonView polygons[geofence_format::kMaximumAreas];
  GeofenceAreaSetView view = s.view(polygons);
  assert(view.area_count == 2);
  assert(polygons[0].vertex_count == 4 && polygons[1].vertex_count == 3);
  assert(polygons[1].vertices[0].latitude_e7 == 100);
  // A trailing empty area is not part of the set.
  assert(s.nextArea() == GeofenceStaging::Result::kOk);
  assert(s.view(polygons).area_count == 2);

  // At most 8 areas.
  s.begin();
  for (int area = 0; area < 8; ++area) {
    if (area != 0) assert(s.nextArea() == GeofenceStaging::Result::kOk);
    s.addVertex(GeoPointE7(area, area));
  }
  assert(s.nextArea() == GeofenceStaging::Result::kTooManyAreas);
  // At most 64 vertices in total.
  s.begin();
  for (int i = 0; i < 64; ++i)
    assert(s.addVertex(GeoPointE7(i, i)) == GeofenceStaging::Result::kOk);
  assert(s.addVertex(GeoPointE7(0, 0)) == GeofenceStaging::Result::kFull);
  s.abort();
  assert(!s.active() && s.vertexCount() == 0);
}

// A ~100 m square near 37.85 N 28.12 E, as staged from USB.
void stageSquare(GeofenceStaging& s, int32_t shift = 0) {
  s.begin();
  s.addVertex(GeoPointE7(378500000 + shift, 281200000));
  s.addVertex(GeoPointE7(378500000 + shift, 281210000));
  s.addVertex(GeoPointE7(378510000 + shift, 281210000));
  s.addVertex(GeoPointE7(378510000 + shift, 281200000));
}

const ApplicationAccessContext kUsb(ApplicationAccessChannel::kUsbLocal);

GeofenceMutationResult finish(GeofenceStore& store, GeofenceMutationOwner& owner,
                              unsigned* applied_signals) {
  GeofenceMutationResult result;
  for (int pass = 0; pass < 64; ++pass) {
    store.poll();
    if (owner.poll()) ++*applied_signals;
    if (owner.takeResult(ApplicationRequester::kUsb, result)) return result;
  }
  assert(false && "geofence mutation never completed");
  return result;
}

void owner() {
  FakeFlash flash;
  FixedIncarnation incarnation;
  GeofenceStore store(flash, &incarnation);
  assert(store.begin());
  assert(store.resourceState() == GeofenceResourceState::kClear);
  GeofenceMutationOwner owner(store);
  GeofenceStaging staging;
  GeofencePolygonView polygons[geofence_format::kMaximumAreas];
  unsigned applied = 0;

  // REPLACE_SET: committed before anything is published.
  stageSquare(staging);
  assert(owner.submitReplace(ApplicationRequester::kUsb, 1, kUsb,
                             staging.view(polygons)) ==
         ConfigMutationSubmitResult::kAccepted);
  assert(owner.busy() && store.busy());
  // One change at a time.
  assert(owner.submitClear(ApplicationRequester::kUsb, 2, kUsb) ==
         ConfigMutationSubmitResult::kBusy);
  GeofenceMutationResult r = finish(store, owner, &applied);
  assert(applied == 1);
  assert(r.request_id == 1 && r.kind == GeofenceMutationKind::kReplace);
  assert(r.outcome == ConfigMutationOutcome::kApplied);
  assert(r.state == GeofenceResourceState::kConfigured);
  assert(r.area_count == 1 && r.total_vertex_count == 4);
  assert(r.token_valid && r.token.revision == 2);

  // Same set again: nothing written.
  uint32_t programs = flash.program_calls, erases = flash.erase_calls;
  stageSquare(staging);
  assert(owner.submitReplace(ApplicationRequester::kUsb, 3, kUsb,
                             staging.view(polygons)) ==
         ConfigMutationSubmitResult::kAccepted);
  r = finish(store, owner, &applied);
  assert(r.outcome == ConfigMutationOutcome::kUnchanged && applied == 1);
  assert(r.token.revision == 2);
  assert(flash.program_calls == programs && flash.erase_calls == erases);

  // Invalid geometry refuses the whole set and reports what is stored.
  const GeoPointE7 bowtie[] = {GeoPointE7(378500000, 281200000),
                               GeoPointE7(378510000, 281210000),
                               GeoPointE7(378500000, 281210000),
                               GeoPointE7(378510000, 281200000)};
  const GeoPointE7 two[] = {GeoPointE7(378500000, 281200000),
                            GeoPointE7(378510000, 281210000)};
  for (const GeofencePolygonView& bad :
       {GeofencePolygonView(bowtie, 4), GeofencePolygonView(two, 2)}) {
    assert(owner.submitReplace(ApplicationRequester::kUsb, 4, kUsb,
                               GeofenceAreaSetView(&bad, 1)) ==
           ConfigMutationSubmitResult::kAccepted);
    r = finish(store, owner, &applied);
    assert(r.outcome == ConfigMutationOutcome::kInvalid);
    assert(r.state == GeofenceResourceState::kConfigured &&
           r.total_vertex_count == 4 && r.token.revision == 2);
  }
  // A valid area next to an invalid one: the whole replacement is refused.
  stageSquare(staging, 50000);
  staging.nextArea();
  for (const GeoPointE7& p : bowtie) staging.addVertex(p);
  assert(owner.submitReplace(ApplicationRequester::kUsb, 5, kUsb,
                             staging.view(polygons)) ==
         ConfigMutationSubmitResult::kAccepted);
  assert(finish(store, owner, &applied).outcome ==
         ConfigMutationOutcome::kInvalid);
  assert(flash.program_calls == programs && flash.erase_calls == erases);

  // BLE is not authorized to change the geofence yet.
  assert(owner.submitClear(ApplicationRequester::kBle, 6,
                           ApplicationAccessContext(
                               ApplicationAccessChannel::kBleEncrypted)) ==
         ConfigMutationSubmitResult::kAccepted);
  GeofenceMutationResult ble;
  for (int i = 0; i < 4 && !owner.takeResult(ApplicationRequester::kBle, ble);
       ++i)
    owner.poll();
  assert(ble.outcome == ConfigMutationOutcome::kAccessDenied);
  // A requester with someone else's access context is turned away.
  assert(owner.submitClear(ApplicationRequester::kUsb, 7,
                           ApplicationAccessContext(
                               ApplicationAccessChannel::kBleOpen)) ==
         ConfigMutationSubmitResult::kRejected);

  // CLEAR_SET.
  assert(owner.submitClear(ApplicationRequester::kUsb, 8, kUsb) ==
         ConfigMutationSubmitResult::kAccepted);
  r = finish(store, owner, &applied);
  assert(r.outcome == ConfigMutationOutcome::kApplied && applied == 2);
  assert(r.state == GeofenceResourceState::kClear && r.area_count == 0 &&
         r.token.revision == 3);

  // Two areas.
  stageSquare(staging);
  staging.nextArea();
  staging.addVertex(GeoPointE7(378600000, 281300000));
  staging.addVertex(GeoPointE7(378600000, 281310000));
  staging.addVertex(GeoPointE7(378610000, 281300000));
  assert(owner.submitReplace(ApplicationRequester::kUsb, 9, kUsb,
                             staging.view(polygons)) ==
         ConfigMutationSubmitResult::kAccepted);
  r = finish(store, owner, &applied);
  assert(r.outcome == ConfigMutationOutcome::kApplied);
  assert(r.area_count == 2 && r.total_vertex_count == 7 && r.token.revision == 4);

  // A cold boot recovers the last committed set and its token.
  GeofenceStore rebooted(flash, &incarnation);
  assert(rebooted.begin());
  assert(rebooted.resourceState() == GeofenceResourceState::kConfigured);
  assert(rebooted.areaCount() == 2 && rebooted.totalVertexCount() == 7);

  // A write that is not confirmed is OUTCOME_UNKNOWN, never APPLIED, and
  // the runtime is not told to change.
  flash.fail_program_at = static_cast<int>(flash.program_calls) + 1;
  stageSquare(staging, 90000);
  assert(owner.submitReplace(ApplicationRequester::kUsb, 10, kUsb,
                             staging.view(polygons)) ==
         ConfigMutationSubmitResult::kAccepted);
  const unsigned before = applied;
  r = finish(store, owner, &applied);
  assert(r.outcome == ConfigMutationOutcome::kOutcomeUnknown);
  assert(applied == before);
}

void unavailable() {
  FakeFlash flash;
  GeofenceStore store(flash);  // never begun
  GeofenceMutationOwner owner(store);
  assert(owner.submitClear(ApplicationRequester::kUsb, 1, kUsb) ==
         ConfigMutationSubmitResult::kAccepted);
  GeofenceMutationResult r;
  assert(owner.takeResult(ApplicationRequester::kUsb, r));
  assert(r.outcome == ConfigMutationOutcome::kUnavailable && !r.token_valid);
  assert(flash.program_calls == 0 && flash.erase_calls == 0);
}

}  // namespace

int main() {
  parsing();
  staging();
  owner();
  unavailable();
  puts("Geofence local writer: parse, staging and mutation owner checks: PASS");
  return 0;
}
