// Alan (geofence) bellegi "bozulmasi" icin hedefli host testi (2026-11-12).
//
// Sahadaki iz (52E16773141A8887, 31 Ekim ve 9 Kasim; 09A462BD4B275BA5, 10 Ekim):
//   APP STORAGE  geofence_maintenance=yes geofence_corrupt=1
//   APP GEOFENCE resource=0 token=2
//   APP FENCE DIAG page=A evidence=SUPPORTED_CORRUPT all_ff=no tail_ff=no
//   APP FENCE DIAG page=B evidence=ERASED all_ff=yes tail_ff=yes
//
// Bu dosya uc seyi gosterir:
//  1. Ayni iz, GeofenceStore hic yazmadan, A sayfasinda onceki (daha buyuk)
//     bir yazilim imajinin kalintisi varken ilk acilista aynen olusur.
//  2. GeofenceStore'un kendi yazma sirasi, her adimda hata/guc kesilmesi
//     olsa bile bu ize ULASAMAZ: commit edilmis sayfa hep kalir, kayit
//     sonrasi kuyruk (564..4095) hep silinmis kalir.
//  3. requestPrepare(): yalniz commit edilmis kayit hic yokken iki sayfayi
//     siler; saglam alan kaydinin uzerine asla calismaz.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "geofence_store.h"
#include "journal_format.h"
#include "storage_config.h"

using namespace orun_tlp;

namespace {

constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kRegionSize =
    kPageSize * storage_config::kGeofenceRegionPages;
constexpr uint32_t kRecordSize = geofence_format::kRecordSize;

// One knob: the Nth mutating call (program or erase, counted together) is
// interrupted. kFail = nothing changes; kPartial = only the first `bytes`
// bytes change; kAfter = the change lands but the call reports failure.
enum class Cut : uint8_t { kNone, kFail, kPartial, kAfter };

class FakeFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  uint32_t mutating_calls = 0;
  uint32_t cut_at = 0;
  Cut cut = Cut::kNone;
  size_t cut_bytes = 0;
  bool dead = false;  // after the cut: power is gone, nothing else lands.

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
    if (dead) return FlashOpResult::kFailed;
    if (data == nullptr || size == 0 || (offset & 3U) != 0 ||
        (size & 3U) != 0 || offset > bytes.size() ||
        size > bytes.size() - offset)
      return FlashOpResult::kFailed;
    for (size_t i = 0; i < size; ++i)
      if (bytes[offset + i] != 0xFF) return FlashOpResult::kFailed;
    const auto* source = static_cast<const uint8_t*>(data);
    const bool hit = ++mutating_calls == cut_at && cut != Cut::kNone;
    size_t count = size;
    if (hit && cut == Cut::kFail) count = 0;
    if (hit && cut == Cut::kPartial) count = cut_bytes < size ? cut_bytes : size;
    for (size_t i = 0; i < count; ++i) bytes[offset + i] &= source[i];
    if (hit) {
      dead = true;
      return FlashOpResult::kFailed;
    }
    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    if (dead || page >= storage_config::kGeofenceRegionPages)
      return FlashOpResult::kFailed;
    const bool hit = ++mutating_calls == cut_at && cut != Cut::kNone;
    size_t count = kPageSize;
    if (hit && cut == Cut::kFail) count = 0;
    if (hit && cut == Cut::kPartial)
      count = cut_bytes < kPageSize ? cut_bytes : kPageSize;
    memset(bytes.data() + size_t(page) * kPageSize, 0xFF, count);
    if (hit) {
      dead = true;
      return FlashOpResult::kFailed;
    }
    return FlashOpResult::kDone;
  }
};

class FixedIncarnation : public GeofenceIncarnationSource {
 public:
  unsigned calls = 0;
  bool generate(uint64_t& incarnation) override {
    ++calls;
    incarnation = 0x1122334455667788ULL;
    return true;
  }
};

// Bytes that look like the end of an older, larger application image: dense,
// never the ORUN geofence magic, running from the page start to `length`.
void plantLeftoverImage(FakeFlash& flash, uint32_t page, uint32_t length) {
  uint32_t state = 0x2F0B5DF0u + page;
  for (uint32_t i = 0; i < length; ++i) {
    state = state * 1664525u + 1013904223u;
    uint8_t value = static_cast<uint8_t>(state >> 24);
    if (value == 0xFF) value = 0x47;
    flash.bytes[page * kPageSize + i] = value;
  }
  flash.bytes[page * kPageSize] = 0x00;  // never "ORG1"
}

bool tailErased(const FakeFlash& flash, uint32_t page) {
  for (uint32_t i = kRecordSize; i < kPageSize; ++i)
    if (flash.bytes[page * kPageSize + i] != 0xFF) return false;
  return true;
}

geofence_format::PageEvidence evidenceOf(const FakeFlash& flash,
                                         uint32_t page) {
  static geofence_format::PageInspection inspection;
  assert(geofence_format::inspectPage(flash.bytes.data() + page * kPageSize,
                                      kRecordSize, inspection));
  return inspection.evidence;
}

bool requestTriangle(GeofenceStore& store, int32_t delta) {
  const GeoPointE7 vertices[] = {
      GeoPointE7(10000000 + delta, 20000000),
      GeoPointE7(10010000 + delta, 20000000),
      GeoPointE7(10000000 + delta, 20010000),
  };
  const GeofencePolygonView polygon(vertices, 3);
  return store.requestReplace(GeofenceAreaSetView(&polygon, 1));
}

void settle(GeofenceStore& store) {
  for (unsigned i = 0; i < 200 && store.busy(); ++i) store.poll();
  store.poll();  // let a pending recovery run
}

// 1. The exact field signature, with zero GeofenceStore writes.
void testLeftoverImageReproducesFieldSignature() {
  FakeFlash flash;
  plantLeftoverImage(flash, 0, 2500);  // old image ended inside page A
  const auto before = flash.bytes;

  FixedIncarnation incarnation;
  GeofenceStore store(flash, &incarnation);
  assert(store.begin());

  // APP STORAGE geofence_ready=yes geofence_maintenance=yes geofence_corrupt=1
  assert(store.ready());
  assert(store.maintenanceResetRequired());
  assert(store.diagnostics().recovery_corruptions == 1);
  // APP GEOFENCE resource=0 token=2
  assert(static_cast<unsigned>(store.resourceState()) == 0);
  assert(static_cast<unsigned>(store.tokenState()) == 2);
  // APP FENCE DIAG page=A SUPPORTED_CORRUPT tail_ff=no / page=B ERASED
  assert(evidenceOf(flash, 0) ==
         geofence_format::PageEvidence::kSupportedCorrupt);
  assert(!tailErased(flash, 0));
  assert(evidenceOf(flash, 1) == geofence_format::PageEvidence::kErased);
  assert(tailErased(flash, 1));

  // The store wrote nothing, made no baseline, and refuses every area write:
  // the collar can never hold an area until someone erases page A.
  assert(flash.mutating_calls == 0);
  assert(incarnation.calls == 0);
  assert(flash.bytes == before);
  assert(!requestTriangle(store, 0));
  assert(!store.requestClear());
  assert(!store.busy());
}

// 2. GeofenceStore's own writes cannot reach that state, whatever is cut.
void assertNeverTheFieldState(const FakeFlash& flash, const char* what,
                              unsigned at) {
  unsigned committed = 0;
  for (uint32_t page = 0; page < 2; ++page) {
    if (geofence_format::isAuthoritativeCommitted(evidenceOf(flash, page)))
      ++committed;
    if (!tailErased(flash, page)) {
      printf("FAIL %s cut_at=%u: page %lu tail written\n", what, at,
             static_cast<unsigned long>(page));
      assert(false);
    }
  }
  if (committed == 0) {
    printf("FAIL %s cut_at=%u: no committed page left\n", what, at);
    assert(false);
  }
}

void testOwnWritesNeverLoseTheCommittedPage() {
  struct Case { Cut cut; size_t bytes; const char* name; };
  const Case cases[] = {
      {Cut::kFail, 0, "op-fails"},
      {Cut::kPartial, 1, "torn-1-byte"},
      {Cut::kPartial, 3, "torn-3-bytes"},
      {Cut::kPartial, 200, "torn-200-bytes"},
      {Cut::kPartial, 559, "torn-559-bytes"},
      {Cut::kAfter, 0, "lands-but-reported-failed"},
  };
  unsigned scenarios = 0;
  for (const Case& c : cases) {
    // Baseline = 2 programs. Each later change = erase + body + commit.
    // Five changes after the baseline cover both A->B and B->A directions.
    for (uint32_t at = 3; at <= 2 + 5 * 3; ++at) {
      FakeFlash flash;
      FixedIncarnation incarnation;
      {
        GeofenceStore store(flash, &incarnation);
        assert(store.begin());
        assert(!store.maintenanceResetRequired());
        flash.cut = c.cut;
        flash.cut_bytes = c.bytes;
        flash.cut_at = at;
        for (int change = 0; change < 5 && !flash.dead; ++change) {
          bool ok = false;
          const bool asked = (change % 2 == 0)
                                 ? requestTriangle(store, change * 100)
                                 : store.requestClear();
          if (!asked) break;
          settle(store);
          store.takeMutationResult(ok);
        }
      }
      assert(flash.dead);  // the cut really happened
      assertNeverTheFieldState(flash, c.name, at);

      // Reboot on the cut flash: the last committed area set is still served.
      flash.dead = false;
      flash.cut = Cut::kNone;
      GeofenceStore reboot(flash, &incarnation);
      assert(reboot.begin());
      assert(reboot.resourceState() != GeofenceResourceState::kUnavailable);
      assertNeverTheFieldState(flash, c.name, at);
      ++scenarios;
    }
  }
  printf("own-write cut scenarios checked: %u\n", scenarios);
}

#ifndef ORUN_TEST_PRISTINE_STORE
// 3. requestPrepare(): the sanctioned way out, and its limits.
void testPrepareOnlyWithoutCommittedRecord() {
  // 3a. Leftover in A: prepare erases both pages; next boot makes the
  // baseline; areas can then be written.
  {
    FakeFlash flash;
    plantLeftoverImage(flash, 0, 2500);
    FixedIncarnation incarnation;
    GeofenceStore store(flash, &incarnation);
    assert(store.begin());
    assert(store.prepareAllowed());
    assert(store.requestPrepare());
    assert(store.busy());
    assert(!requestTriangle(store, 0));  // nothing else while preparing
    settle(store);
    bool prepared = false;
    assert(store.takePrepareResult(prepared) && prepared);
    assert(!store.takePrepareResult(prepared));  // exactly once
    for (uint8_t byte : flash.bytes) assert(byte == 0xFF);
    assert(!store.prepareAllowed());
    assert(!store.requestPrepare());  // nothing left to prepare

    GeofenceStore reboot(flash, &incarnation);
    assert(reboot.begin());
    assert(!reboot.maintenanceResetRequired());
    assert(reboot.resourceState() == GeofenceResourceState::kClear);
    assert(reboot.tokenState() == GeofenceTokenState::kValid);
    assert(requestTriangle(reboot, 0));
    settle(reboot);
    bool ok = false;
    assert(reboot.takeMutationResult(ok) && ok);
    assert(reboot.areaCount() == 1);
  }

  // 3b. Leftover across both pages (older image ended above page B).
  {
    FakeFlash flash;
    plantLeftoverImage(flash, 0, kPageSize);
    plantLeftoverImage(flash, 1, 900);
    FixedIncarnation incarnation;
    GeofenceStore store(flash, &incarnation);
    assert(store.begin());
    assert(store.diagnostics().recovery_corruptions == 2);
    assert(store.requestPrepare());
    settle(store);
    bool prepared = false;
    assert(store.takePrepareResult(prepared) && prepared);
    for (uint8_t byte : flash.bytes) assert(byte == 0xFF);
  }

  // 3c. A healthy store is never preparable: CLEAR baseline, and configured.
  {
    FakeFlash flash;
    FixedIncarnation incarnation;
    GeofenceStore store(flash, &incarnation);
    assert(store.begin());
    assert(!store.prepareAllowed());
    assert(!store.requestPrepare());
    assert(requestTriangle(store, 0));
    settle(store);
    bool ok = false;
    assert(store.takeMutationResult(ok) && ok);
    const auto before = flash.bytes;
    const uint32_t calls = flash.mutating_calls;
    assert(!store.prepareAllowed());
    assert(!store.requestPrepare());
    assert(flash.mutating_calls == calls && flash.bytes == before);
  }

  // 3d. A committed area set with a damaged neighbour (maintenance lockout,
  // but the owner's areas are still readable) is NOT preparable.
  {
    FakeFlash flash;
    FixedIncarnation incarnation;
    {
      GeofenceStore store(flash, &incarnation);
      assert(store.begin());
      assert(requestTriangle(store, 0));
      settle(store);
      bool ok = false;
      assert(store.takeMutationResult(ok) && ok);  // areas now on page B
    }
    flash.bytes[0 * kPageSize + 2000] = 0x00;  // dirt in page A's tail
    GeofenceStore reboot(flash, &incarnation);
    assert(reboot.begin());
    assert(reboot.maintenanceResetRequired());
    assert(reboot.areaCount() == 1);  // still served read-only
    const auto before = flash.bytes;
    assert(!reboot.prepareAllowed());
    assert(!reboot.requestPrepare());
    assert(flash.bytes == before);
  }

  // 3e. Erase failure during prepare: reported FAIL, never claims success,
  // and the request can be repeated.
  {
    FakeFlash flash;
    plantLeftoverImage(flash, 0, 2500);
    FixedIncarnation incarnation;
    GeofenceStore store(flash, &incarnation);
    assert(store.begin());
    flash.cut = Cut::kFail;
    flash.cut_at = 1;
    assert(store.requestPrepare());
    settle(store);
    bool prepared = true;
    assert(store.takePrepareResult(prepared) && !prepared);
    assert(store.maintenanceResetRequired());

    flash.dead = false;
    flash.cut = Cut::kNone;
    assert(store.prepareAllowed());
    assert(store.requestPrepare());
    settle(store);
    assert(store.takePrepareResult(prepared) && prepared);
    for (uint8_t byte : flash.bytes) assert(byte == 0xFF);
  }

  // 3f. Power lost between the two erases: next boot is still preparable or
  // already blank; either way no half state is taken for a valid area set.
  {
    FakeFlash flash;
    plantLeftoverImage(flash, 0, kPageSize);
    plantLeftoverImage(flash, 1, 900);
    FixedIncarnation incarnation;
    {
      GeofenceStore store(flash, &incarnation);
      assert(store.begin());
      flash.cut = Cut::kPartial;
      flash.cut_bytes = 100;
      flash.cut_at = 2;  // second erase torn
      assert(store.requestPrepare());
      settle(store);
    }
    flash.dead = false;
    flash.cut = Cut::kNone;
    GeofenceStore reboot(flash, &incarnation);
    assert(reboot.begin());
    assert(reboot.resourceState() == GeofenceResourceState::kUnavailable);
    assert(reboot.maintenanceResetRequired());
    assert(reboot.requestPrepare());
    settle(reboot);
    bool prepared = false;
    assert(reboot.takePrepareResult(prepared) && prepared);
  }

  // 3g. A page written by a newer format is never erased by prepare.
  {
    FakeFlash flash;
    FixedIncarnation incarnation;
    {
      GeofenceStore store(flash, &incarnation);
      assert(store.begin());  // real v1 baseline on page A
    }
    static uint8_t page[kPageSize];
    memcpy(page, flash.bytes.data(), kPageSize);
    page[4] = 4;  // same magic, a version this firmware does not know
    memset(flash.bytes.data(), 0xFF, kPageSize);
    memcpy(flash.bytes.data(), page, kRecordSize);
    if (evidenceOf(flash, 0) ==
        geofence_format::PageEvidence::kUnsupportedNewer) {
      GeofenceStore reboot(flash, &incarnation);
      assert(reboot.begin());
      assert(reboot.maintenanceResetRequired());
      const auto before = flash.bytes;
      assert(!reboot.prepareAllowed());
      assert(!reboot.requestPrepare());
      assert(flash.bytes == before);
    } else {
      printf("note: newer-version page not classified UNSUPPORTED_NEWER\n");
      assert(false);
    }
  }

  // 3h. On the device the erase goes through FlashMutationGate and completes
  // later (SoftDevice running): accepted now, done on a later pass.
  {
    class LateFlash : public FakeFlash {
     public:
      int pending_page = -1;
      bool lose_completion = false;
      FlashOpResult erasePage(uint32_t page) override {
        pending_page = static_cast<int>(page);
        return FlashOpResult::kPending;
      }
      FlashOpResult pollPending() override {
        if (pending_page < 0) return FlashOpResult::kFailed;
        if (lose_completion) return FlashOpResult::kFailed;  // gate timeout
        const uint32_t page = static_cast<uint32_t>(pending_page);
        pending_page = -1;
        return FakeFlash::erasePage(page);
      }
      bool hasUnreconciledMutation() const override {
        return lose_completion && pending_page >= 0;
      }
    };
    LateFlash flash;
    plantLeftoverImage(flash, 0, 2500);
    FixedIncarnation incarnation;
    GeofenceStore store(flash, &incarnation);
    assert(store.begin());
    assert(store.requestPrepare());
    store.poll();  // erase A accepted
    assert(store.busy() && flash.bytes[0] != 0xFF);
    bool prepared = false;
    assert(!store.takePrepareResult(prepared));
    settle(store);
    assert(store.takePrepareResult(prepared) && prepared);
    for (uint8_t byte : flash.bytes) assert(byte == 0xFF);

    // Accepted erase whose completion never arrives in time: FAIL, and no
    // new request is admitted until the gate has reconciled the operation.
    LateFlash stuck;
    plantLeftoverImage(stuck, 0, 2500);
    GeofenceStore second(stuck, &incarnation);
    assert(second.begin());
    assert(second.requestPrepare());
    second.poll();
    stuck.lose_completion = true;
    second.poll();
    prepared = true;
    assert(second.takePrepareResult(prepared) && !prepared);
    second.poll();
    assert(!second.prepareAllowed() && !second.requestPrepare());
    stuck.lose_completion = false;  // the late completion is reconciled
    stuck.pending_page = -1;
    second.poll();
    second.poll();
    assert(second.prepareAllowed());
  }
}

#endif  // ORUN_TEST_PRISTINE_STORE

}  // namespace

int main() {
  testLeftoverImageReproducesFieldSignature();
  testOwnWritesNeverLoseTheCommittedPage();
#ifndef ORUN_TEST_PRISTINE_STORE
  testPrepareOnlyWithoutCommittedRecord();
#endif
  printf("geofence foreign-leftover tests PASS\n");
  return 0;
}
