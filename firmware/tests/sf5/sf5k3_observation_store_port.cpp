#include "sf5k3_observation_store_port.h"
#include <string.h>

namespace orun_tlp {
namespace sf5k3_candidate {
namespace {
bool sameId(const osf::RecordIdentity& a, const osf::RecordIdentity& b) {
  return a.incarnation == b.incarnation && a.sequence == b.sequence;
}
}

ObservationStorePort::ObservationStorePort(ObservationStore& store,
    const osc::ExactObject* expected, size_t count)
    : store_(store) {
  if (!expected || count == 0U || count > kMaxMembers) return;
  for (size_t i = 0U; i < count; ++i) {
    const osc::ExactObject& e = expected[i];
    if (!e.identity.valid() || e.object_size == 0U ||
        e.object_size > osc::kMaxProtectedObjectSize ||
        (e.record_kind != osf::RecordKind::kPeriodic &&
         e.record_kind != osf::RecordKind::kEvent)) return;
    for (size_t j = 0U; j < i; ++j)
      if (sameId(e.identity, expected[j].identity)) return;
  }
  for (size_t i = 0U; i < count; ++i) expected_[i] = expected[i];
  count_ = count;
  ready_ = true;
}

const osc::ExactObject* ObservationStorePort::expectedFor(
    const osf::RecordIdentity& id) const {
  for (size_t i = 0U; i < count_; ++i)
    if (sameId(expected_[i].identity, id)) return &expected_[i];
  return nullptr;
}

bool ObservationStorePort::inspect(const osf::RecordIdentity& id,
                                   DurableView& out) {
  out = DurableView{};
  const osc::ExactObject* want = ready_ ? expectedFor(id) : nullptr;
  if (!want || !store_.ready() || store_.faulted()) return false;

  ObservationStore::Record rec{};
  if (store_.lookup(id, rec) != ObservationStore::LookupResult::kFound)
    return false;
  if (rec.kind != want->record_kind) return false;

  out.record_present = true;
  out.released = rec.released;
  out.mutation_uncertain = rec.release_uncertain ||
                           rec.release_marker_exhausted;

  osc::ExactObject actual{};
  const ObservationStore::ControlLookupResult found =
      store_.findExactObject(id, actual);
  if (found == ObservationStore::ControlLookupResult::kReadError)
    return false;
  out.exact_present = found == ObservationStore::ControlLookupResult::kFound;
  if (out.exact_present)
    out.exact_matches =
        sameId(actual.identity, want->identity) &&
        actual.record_kind == want->record_kind &&
        actual.object_size == want->object_size &&
        memcmp(actual.object, want->object, want->object_size) == 0;
  return true;
}

Request ObservationStorePort::requestRelease(const osf::RecordIdentity& id) {
  if (!ready_ || !expectedFor(id) || !store_.ready() || store_.faulted())
    return Request::kRejected;
  if (store_.busy()) return Request::kBusy;
  return store_.requestRelease(id) ? Request::kStarted : Request::kRejected;
}

bool ObservationStorePort::takeReleaseResult(bool& success) {
  return store_.takeReleaseResult(success);
}
} // namespace sf5k3_candidate
} // namespace orun_tlp
