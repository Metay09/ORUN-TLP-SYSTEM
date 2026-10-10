// ConfigMutationOwner against the production ConfigStore.
//
// A portable fake FlashBackend models the two-page ConfigStore region with NOR
// semantics and optional asynchronous completion/failure, so every outcome a
// writer can report is driven through the real store, not a mock of it.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <array>

#include "config_mutation.h"
#include "config_store.h"
#include "gnss_config.h"
#include "storage_config.h"

using namespace orun_tlp;

namespace {

constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kRegionSize =
    kPageSize * storage_config::kFutureConfigRegionPages;

class FixedIncarnation : public ConfigIncarnationSource {
 public:
  bool generate(uint64_t& incarnation) override {
    incarnation = 0x1122334455667788ULL;
    return true;
  }
};

class FakeFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  uint32_t pending_steps = 0;
  bool fail_on_resolve = false;
  uint32_t program_calls = 0, erase_calls = 0;

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
    assert(!in_flight_);
    if (data == nullptr || size == 0 || (offset & 3U) != 0 ||
        (size & 3U) != 0 || offset > bytes.size() ||
        size > bytes.size() - offset)
      return FlashOpResult::kFailed;
    const auto* source = static_cast<const uint8_t*>(data);
    // NOR flash can only clear bits.
    for (size_t index = 0; index < size; ++index)
      if ((bytes[offset + index] & source[index]) != source[index])
        return FlashOpResult::kFailed;
    for (size_t index = 0; index < size; ++index)
      bytes[offset + index] &= source[index];
    return start();
  }

  FlashOpResult erasePage(uint32_t page) override {
    ++erase_calls;
    assert(!in_flight_);
    if (page >= storage_config::kFutureConfigRegionPages)
      return FlashOpResult::kFailed;
    memset(bytes.data() + size_t(page) * kPageSize, 0xFF, kPageSize);
    return start();
  }

  FlashOpResult pollPending() override {
    assert(in_flight_);
    if (remaining_ > 0) {
      --remaining_;
      return FlashOpResult::kPending;
    }
    in_flight_ = false;
    return fail_on_resolve ? FlashOpResult::kFailed : FlashOpResult::kDone;
  }

 private:
  FlashOpResult start() {
    if (pending_steps == 0)
      return fail_on_resolve ? FlashOpResult::kFailed : FlashOpResult::kDone;
    remaining_ = pending_steps - 1;
    in_flight_ = true;
    return FlashOpResult::kPending;
  }

  uint32_t remaining_ = 0;
  bool in_flight_ = false;
};

ConfigMutationRequest usbInterval(uint32_t id, uint32_t seconds) {
  return ConfigMutationRequest(
      ApplicationRequester::kUsb, id,
      ApplicationAccessContext(ApplicationAccessChannel::kUsbLocal),
      ConfigMutationKind::kSetTrackingInterval, seconds);
}

ConfigMutationRequest usbServices(uint32_t id, uint8_t mode,
                                  uint8_t services) {
  return ConfigMutationRequest(
      ApplicationRequester::kUsb, id,
      ApplicationAccessContext(ApplicationAccessChannel::kUsbLocal),
      ConfigMutationKind::kSetServiceIntent, 0, mode, services);
}

struct Completion {
  ConfigMutationResult result;
  uint32_t applied_signals = 0;
};

// Drives store and owner the way loop() does until the result is takeable.
Completion finish(ConfigStore& store, ConfigMutationOwner& owner,
                  ApplicationRequester requester = ApplicationRequester::kUsb) {
  Completion completion;
  for (int pass = 0; pass < 64; ++pass) {
    store.poll();
    if (owner.poll()) ++completion.applied_signals;
    if (owner.takeResult(requester, completion.result)) return completion;
  }
  assert(false && "mutation never completed");
  return completion;
}

void appliedChangeIsDurableAndAdvancesTheTokenOnce() {
  FakeFlash flash;
  FixedIncarnation incarnation;
  ConfigStore store(flash, &incarnation);
  assert(store.begin());
  config_format::StateToken baseline;
  assert(store.stateToken(baseline) && baseline.revision == 1);
  assert(!store.hasCommittedRecord());
  const uint32_t default_interval = store.config().tracking_interval_seconds;
  assert(default_interval == gnss_config::kTrackingIntervalSeconds);

  ConfigMutationOwner owner(store);
  assert(!owner.busy());
  assert(owner.submit(usbInterval(7, 900)) ==
         ConfigMutationSubmitResult::kAccepted);
  // Nothing is published before the durable commit.
  assert(owner.busy() && !owner.resultPending());
  assert(store.config().tracking_interval_seconds == default_interval);

  const Completion done = finish(store, owner);
  assert(done.applied_signals == 1);
  assert(done.result.requester == ApplicationRequester::kUsb);
  assert(done.result.request_id == 7);
  assert(done.result.outcome == ConfigMutationOutcome::kApplied);
  assert(done.result.config.tracking_interval_seconds == 900);
  assert(done.result.token_valid);
  assert(done.result.token.incarnation == baseline.incarnation);
  assert(done.result.token.revision == baseline.revision + 1);
  assert(!owner.busy());
  assert(store.hasCommittedRecord());
  // The untouched field is carried over.
  assert(done.result.config.battery_capacity_mah ==
         store.config().battery_capacity_mah);

  // A cold boot on the same flash recovers the change and the same token.
  ConfigStore rebooted(flash, &incarnation);
  assert(rebooted.begin());
  assert(rebooted.config().tracking_interval_seconds == 900);
  config_format::StateToken recovered;
  assert(rebooted.stateToken(recovered));
  assert(recovered.incarnation == done.result.token.incarnation &&
         recovered.revision == done.result.token.revision);
}

void unchangedValueWritesNothing() {
  FakeFlash flash;
  FixedIncarnation incarnation;
  ConfigStore store(flash, &incarnation);
  assert(store.begin());
  ConfigMutationOwner owner(store);
  assert(owner.submit(usbInterval(1, 900)) ==
         ConfigMutationSubmitResult::kAccepted);
  const Completion first = finish(store, owner);
  assert(first.result.outcome == ConfigMutationOutcome::kApplied);

  const uint32_t programs = flash.program_calls, erases = flash.erase_calls;
  assert(owner.submit(usbInterval(2, 900)) ==
         ConfigMutationSubmitResult::kAccepted);
  // Already satisfied: the result is ready at once, with no apply signal.
  assert(owner.resultPending());
  const Completion second = finish(store, owner);
  assert(second.applied_signals == 0);
  assert(second.result.request_id == 2);
  assert(second.result.outcome == ConfigMutationOutcome::kUnchanged);
  assert(second.result.token_valid &&
         second.result.token.revision == first.result.token.revision);
  assert(second.result.config.tracking_interval_seconds == 900);
  assert(flash.program_calls == programs && flash.erase_calls == erases);
}

void outOfRangeValuesAreRefusedWithoutWriting() {
  FakeFlash flash;
  FixedIncarnation incarnation;
  ConfigStore store(flash, &incarnation);
  assert(store.begin());
  ConfigMutationOwner owner(store);
  const uint32_t programs = flash.program_calls, erases = flash.erase_calls;
  const uint32_t before = store.config().tracking_interval_seconds;

  const uint32_t rejected[] = {
      0, 1, config_mutation_policy::kMinTrackingIntervalSeconds - 1,
      gnss_config::kMaxTrackingIntervalSeconds + 1, UINT32_MAX};
  uint32_t id = 10;
  for (const uint32_t value : rejected) {
    assert(owner.submit(usbInterval(id, value)) ==
           ConfigMutationSubmitResult::kAccepted);
    const Completion done = finish(store, owner);
    assert(done.applied_signals == 0);
    assert(done.result.request_id == id);
    assert(done.result.outcome == ConfigMutationOutcome::kInvalid);
    // The result reports what the store holds, not what was asked for.
    assert(done.result.config.tracking_interval_seconds == before);
    assert(done.result.token_valid && done.result.token.revision == 1);
    ++id;
  }
  assert(flash.program_calls == programs && flash.erase_calls == erases);

  // Both ends of the admitted range are accepted.
  assert(owner.submit(usbInterval(
             20, config_mutation_policy::kMinTrackingIntervalSeconds)) ==
         ConfigMutationSubmitResult::kAccepted);
  assert(finish(store, owner).result.outcome ==
         ConfigMutationOutcome::kApplied);
  assert(owner.submit(usbInterval(
             21, gnss_config::kMaxTrackingIntervalSeconds)) ==
         ConfigMutationSubmitResult::kAccepted);
  const Completion upper = finish(store, owner);
  assert(upper.result.outcome == ConfigMutationOutcome::kApplied);
  assert(upper.result.config.tracking_interval_seconds ==
         gnss_config::kMaxTrackingIntervalSeconds);
  assert(upper.result.token.revision == 3);
}

void oneChangeAtATimeAcrossRequesters() {
  FakeFlash flash;
  FixedIncarnation incarnation;
  ConfigStore store(flash, &incarnation);
  assert(store.begin());
  flash.pending_steps = 3;  // keep the save in flight for several passes
  ConfigMutationOwner owner(store);

  assert(owner.submit(usbInterval(1, 600)) ==
         ConfigMutationSubmitResult::kAccepted);
  // A second writer is told BUSY; it is never queued behind the first.
  assert(owner.submit(usbInterval(2, 700)) ==
         ConfigMutationSubmitResult::kBusy);
  store.poll();
  assert(!owner.poll());
  assert(owner.submit(usbInterval(3, 700)) ==
         ConfigMutationSubmitResult::kBusy);

  // Complete the save but do not take the result yet: the slot stays held.
  ConfigMutationResult result;
  for (int pass = 0; pass < 64 && !owner.resultPending(); ++pass) {
    store.poll();
    owner.poll();
  }
  assert(owner.resultPending());
  assert(owner.submit(usbInterval(4, 700)) ==
         ConfigMutationSubmitResult::kBusy);
  // Another requester cannot consume the USB result.
  assert(!owner.takeResult(ApplicationRequester::kBle, result));
  assert(owner.resultPending());
  assert(owner.takeResult(ApplicationRequester::kUsb, result));
  assert(result.request_id == 1 &&
         result.outcome == ConfigMutationOutcome::kApplied &&
         result.config.tracking_interval_seconds == 600);
  assert(!owner.takeResult(ApplicationRequester::kUsb, result));

  // The refused requests left no trace; the slot is free again.
  assert(owner.submit(usbInterval(5, 700)) ==
         ConfigMutationSubmitResult::kAccepted);
  assert(finish(store, owner).result.config.tracking_interval_seconds == 700);
}

void onlyLocalUsbMayChangeConfiguration() {
  FakeFlash flash;
  FixedIncarnation incarnation;
  ConfigStore store(flash, &incarnation);
  assert(store.begin());
  ConfigMutationOwner owner(store);
  const uint32_t programs = flash.program_calls, erases = flash.erase_calls;

  const ApplicationAccessChannel ble_channels[] = {
      ApplicationAccessChannel::kBleOpen,
      ApplicationAccessChannel::kBleEncrypted};
  for (const ApplicationAccessChannel channel : ble_channels) {
    assert(owner.submit(ConfigMutationRequest(
               ApplicationRequester::kBle, 50,
               ApplicationAccessContext(channel),
               ConfigMutationKind::kSetTrackingInterval, 900)) ==
           ConfigMutationSubmitResult::kAccepted);
    // USB cannot drain a BLE-owned result.
    ConfigMutationResult stray;
    assert(!owner.takeResult(ApplicationRequester::kUsb, stray));
    const Completion done = finish(store, owner, ApplicationRequester::kBle);
    assert(done.result.requester == ApplicationRequester::kBle);
    assert(done.result.outcome == ConfigMutationOutcome::kAccessDenied);
    assert(done.applied_signals == 0);
  }

  // An access context that cannot belong to its requester is rejected before
  // the slot is taken.
  assert(owner.submit(ConfigMutationRequest(
             ApplicationRequester::kUsb, 51,
             ApplicationAccessContext(ApplicationAccessChannel::kBleEncrypted),
             ConfigMutationKind::kSetTrackingInterval, 900)) ==
         ConfigMutationSubmitResult::kRejected);
  assert(owner.submit(ConfigMutationRequest(
             ApplicationRequester::kBle, 52,
             ApplicationAccessContext(ApplicationAccessChannel::kUsbLocal),
             ConfigMutationKind::kSetTrackingInterval, 900)) ==
         ConfigMutationSubmitResult::kRejected);
  assert(owner.submit(ConfigMutationRequest(
             ApplicationRequester::kUsb, 53,
             ApplicationAccessContext(ApplicationAccessChannel::kInvalid),
             ConfigMutationKind::kSetTrackingInterval, 900)) ==
         ConfigMutationSubmitResult::kRejected);
  assert(!owner.busy());

  assert(flash.program_calls == programs && flash.erase_calls == erases);
  assert(store.config().tracking_interval_seconds ==
         gnss_config::kTrackingIntervalSeconds);
}

void unconfirmedSaveIsOutcomeUnknownNeverApplied() {
  FakeFlash flash;
  FixedIncarnation incarnation;
  ConfigStore store(flash, &incarnation);
  assert(store.begin());
  ConfigMutationOwner owner(store);
  const uint32_t before = store.config().tracking_interval_seconds;

  flash.fail_on_resolve = true;
  assert(owner.submit(usbInterval(1, 900)) ==
         ConfigMutationSubmitResult::kAccepted);
  const Completion failed = finish(store, owner);
  // No runtime-apply signal and no false FAILED: the store could not confirm.
  assert(failed.applied_signals == 0);
  assert(failed.result.outcome == ConfigMutationOutcome::kOutcomeUnknown);
  assert(!failed.result.token_valid);
  assert(failed.result.token.incarnation == 0 &&
         failed.result.token.revision == 0);
  assert(failed.result.config.tracking_interval_seconds == before);
  assert(!owner.busy());

  // Once flash works again the store reconciles and a new change goes
  // through; the earlier unconfirmed value was never published.
  flash.fail_on_resolve = false;
  for (int pass = 0; pass < 8; ++pass) store.poll();
  assert(store.config().tracking_interval_seconds == before);
  assert(owner.submit(usbInterval(2, 1200)) ==
         ConfigMutationSubmitResult::kAccepted);
  const Completion retried = finish(store, owner);
  assert(retried.result.outcome == ConfigMutationOutcome::kApplied);
  assert(retried.result.config.tracking_interval_seconds == 1200);
  assert(retried.applied_signals == 1);
}

void storeStatesMapToHonestOutcomes() {
  // Not started: unavailable.
  {
    FakeFlash flash;
    FixedIncarnation incarnation;
    ConfigStore store(flash, &incarnation);
    ConfigMutationOwner owner(store);
    assert(owner.submit(usbInterval(1, 900)) ==
           ConfigMutationSubmitResult::kAccepted);
    const Completion done = finish(store, owner);
    assert(done.result.outcome == ConfigMutationOutcome::kUnavailable);
    assert(!done.result.token_valid);
  }

  // Evidence the store will not overwrite on its own: maintenance.
  {
    FakeFlash flash;
    memset(flash.bytes.data(), 0x00, 64);  // programmed, not a valid record
    FixedIncarnation incarnation;
    ConfigStore store(flash, &incarnation);
    assert(store.begin());
    assert(store.maintenanceResetRequired());
    ConfigMutationOwner owner(store);
    const uint32_t programs = flash.program_calls, erases = flash.erase_calls;
    assert(owner.submit(usbInterval(2, 900)) ==
           ConfigMutationSubmitResult::kAccepted);
    const Completion done = finish(store, owner);
    assert(done.result.outcome == ConfigMutationOutcome::kMaintenance);
    assert(done.applied_signals == 0);
    assert(flash.program_calls == programs && flash.erase_calls == erases);
  }

  // Another writer already has a save in flight: BUSY, slot not held.
  {
    FakeFlash flash;
    FixedIncarnation incarnation;
    ConfigStore store(flash, &incarnation);
    assert(store.begin());
    flash.pending_steps = 3;
    config_format::Config other = store.config();
    other.tracking_interval_seconds = 300;
    assert(store.requestSave(other));
    ConfigMutationOwner owner(store);
    assert(owner.submit(usbInterval(3, 900)) ==
           ConfigMutationSubmitResult::kBusy);
    assert(!owner.busy());
    // That writer's untaken result also blocks a different change.
    for (int pass = 0; pass < 64 && store.busy(); ++pass) store.poll();
    assert(owner.submit(usbInterval(4, 900)) ==
           ConfigMutationSubmitResult::kBusy);
    bool success = false;
    assert(store.takeSaveResult(success) && success);
    assert(owner.submit(usbInterval(5, 900)) ==
           ConfigMutationSubmitResult::kAccepted);
    assert(finish(store, owner).result.outcome ==
           ConfigMutationOutcome::kApplied);
  }

  // The store finished but someone else consumed the result: the owner does
  // not wait forever and does not claim success.
  {
    FakeFlash flash;
    FixedIncarnation incarnation;
    ConfigStore store(flash, &incarnation);
    assert(store.begin());
    ConfigMutationOwner owner(store);
    assert(owner.submit(usbInterval(6, 900)) ==
           ConfigMutationSubmitResult::kAccepted);
    for (int pass = 0; pass < 64 && store.busy(); ++pass) store.poll();
    bool success = false;
    assert(store.takeSaveResult(success));
    const Completion done = finish(store, owner);
    assert(done.applied_signals == 0);
    assert(done.result.outcome == ConfigMutationOutcome::kOutcomeUnknown);
  }
}

void typedAdmissionMatchesTheBoolSeam() {
  FakeFlash flash;
  FixedIncarnation incarnation;
  ConfigStore store(flash, &incarnation);
  config_format::Config candidate(900, 0);
  assert(store.admitSave(candidate) == ConfigSaveAdmission::kUnavailable);
  assert(!store.requestSave(candidate));
  assert(store.begin());

  assert(store.admitSave(store.config()) == ConfigSaveAdmission::kUnchanged);
  assert(store.requestSave(store.config()));
  assert(store.admitSave(config_format::Config(0, 0)) ==
         ConfigSaveAdmission::kInvalid);
  assert(!store.requestSave(config_format::Config(0, 0)));
  assert(store.diagnostics().rejected_candidates == 2);
  assert(store.diagnostics().skipped_unchanged == 2);

  flash.pending_steps = 2;
  assert(store.admitSave(candidate) == ConfigSaveAdmission::kStarted);
  assert(store.admitSave(candidate) == ConfigSaveAdmission::kBusy);
  assert(!store.requestSave(candidate));
  for (int pass = 0; pass < 64 && store.busy(); ++pass) store.poll();
  // Result not taken yet: a different change is still BUSY.
  assert(store.admitSave(config_format::Config(1200, 0)) ==
         ConfigSaveAdmission::kBusy);
  assert(store.diagnostics().blocked_pending_result == 1);
  bool success = false;
  assert(store.takeSaveResult(success) && success);
  assert(store.config().tracking_interval_seconds == 900);
}

}  // namespace

void serviceIntentIsDurableAndOnlyRunnableCombinationsAreAdmitted() {
  namespace cf = config_format;
  FakeFlash flash;
  FixedIncarnation incarnation;
  ConfigStore store(flash, &incarnation);
  assert(store.begin());
  assert(store.config().service_mode == cf::kServiceModeAuto &&
         store.config().requested_services == 0);
  ConfigMutationOwner owner(store);
  assert(owner.submit(usbInterval(1, 900)) ==
         ConfigMutationSubmitResult::kAccepted);
  assert(finish(store, owner).result.outcome == ConfigMutationOutcome::kApplied);

  // Explicit tracking: durable, interval carried over, token advances once.
  assert(owner.submit(usbServices(2, cf::kServiceModeExplicit,
                                  cf::kServiceTracking)) ==
         ConfigMutationSubmitResult::kAccepted);
  const Completion tracking = finish(store, owner);
  assert(tracking.applied_signals == 1);
  assert(tracking.result.kind == ConfigMutationKind::kSetServiceIntent);
  assert(tracking.result.outcome == ConfigMutationOutcome::kApplied);
  assert(tracking.result.config.service_mode == cf::kServiceModeExplicit);
  assert(tracking.result.config.requested_services == cf::kServiceTracking);
  assert(tracking.result.config.tracking_interval_seconds == 900);
  assert(tracking.result.token.revision == 3);
  ConfigStore rebooted(flash, &incarnation);
  assert(rebooted.begin());
  assert(rebooted.config().service_mode == cf::kServiceModeExplicit &&
         rebooted.config().requested_services == cf::kServiceTracking &&
         rebooted.config().tracking_interval_seconds == 900);

  // Same intent again: nothing written.
  uint32_t programs = flash.program_calls, erases = flash.erase_calls;
  assert(owner.submit(usbServices(3, cf::kServiceModeExplicit,
                                  cf::kServiceTracking)) ==
         ConfigMutationSubmitResult::kAccepted);
  const Completion same = finish(store, owner);
  assert(same.result.outcome == ConfigMutationOutcome::kUnchanged &&
         same.applied_signals == 0 && same.result.token.revision == 3);
  assert(flash.program_calls == programs && flash.erase_calls == erases);

  // Refused without writing: combinations this runtime cannot run, AUTO with
  // services, unknown mode, unknown service bits. The result reports what
  // the store holds.
  const struct {
    uint8_t mode, services;
  } refused[] = {
      {cf::kServiceModeExplicit,
       cf::kServiceApplicationReceive | cf::kServiceTracking},
      {cf::kServiceModeExplicit,
       cf::kServiceApplicationReceive | cf::kServiceRelayForwarding},
      {cf::kServiceModeExplicit, cf::kKnownServicesMask},
      {cf::kServiceModeAuto, cf::kServiceTracking},
      {2, cf::kServiceTracking},
      {cf::kServiceModeExplicit, 0x08},
  };
  uint32_t id = 10;
  for (const auto& r : refused) {
    assert(owner.submit(usbServices(id, r.mode, r.services)) ==
           ConfigMutationSubmitResult::kAccepted);
    const Completion done = finish(store, owner);
    assert(done.result.outcome == ConfigMutationOutcome::kInvalid);
    assert(done.applied_signals == 0 && done.result.request_id == id);
    assert(done.result.config.service_mode == cf::kServiceModeExplicit &&
           done.result.config.requested_services == cf::kServiceTracking);
    ++id;
  }
  assert(flash.program_calls == programs && flash.erase_calls == erases);

  // Every runnable intent is accepted, including back to AUTO.
  const struct {
    uint8_t mode, services;
  } accepted[] = {
      {cf::kServiceModeExplicit, 0},
      {cf::kServiceModeExplicit, cf::kServiceRelayForwarding},
      {cf::kServiceModeExplicit,
       cf::kServiceTracking | cf::kServiceRelayForwarding},
      {cf::kServiceModeExplicit, cf::kServiceApplicationReceive},
      {cf::kServiceModeAuto, 0},
  };
  for (const auto& a : accepted) {
    assert(owner.submit(usbServices(id++, a.mode, a.services)) ==
           ConfigMutationSubmitResult::kAccepted);
    const Completion done = finish(store, owner);
    assert(done.result.outcome == ConfigMutationOutcome::kApplied);
    assert(done.result.config.service_mode == a.mode &&
           done.result.config.requested_services == a.services);
    assert(done.result.config.tracking_interval_seconds == 900);
  }

  // An interval change keeps the stored intent.
  assert(owner.submit(usbServices(id++, cf::kServiceModeExplicit,
                                  cf::kServiceRelayForwarding)) ==
         ConfigMutationSubmitResult::kAccepted);
  assert(finish(store, owner).result.outcome == ConfigMutationOutcome::kApplied);
  assert(owner.submit(usbInterval(id++, 600)) ==
         ConfigMutationSubmitResult::kAccepted);
  const Completion interval = finish(store, owner);
  assert(interval.result.outcome == ConfigMutationOutcome::kApplied);
  assert(interval.result.config.service_mode == cf::kServiceModeExplicit &&
         interval.result.config.requested_services ==
             cf::kServiceRelayForwarding);
}

int main() {
  appliedChangeIsDurableAndAdvancesTheTokenOnce();
  unchangedValueWritesNothing();
  outOfRangeValuesAreRefusedWithoutWriting();
  oneChangeAtATimeAcrossRequesters();
  onlyLocalUsbMayChangeConfiguration();
  unconfirmedSaveIsOutcomeUnknownNeverApplied();
  storeStatesMapToHonestOutcomes();
  typedAdmissionMatchesTheBoolSeam();
  serviceIntentIsDurableAndOnlyRunnableCombinationsAreAdmitted();
  puts("Config write path: mutation owner and typed admission checks: PASS");
}
