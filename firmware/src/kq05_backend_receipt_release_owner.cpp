#include "kq05_backend_receipt_release_owner.h"

#include <string.h>

#include "product_record_store.h"

namespace orun_tlp {
namespace {
void wipe(void* ptr, size_t length) {
  volatile uint8_t* data = static_cast<volatile uint8_t*>(ptr);
  while (length-- != 0U) *data++ = 0U;
}
bool usable(const ObservationStore::Record& record,
            const osf::RecordIdentity& identity) {
  return record.handle.identity.incarnation == identity.incarnation &&
         record.handle.identity.sequence == identity.sequence &&
         record.handle.identity.valid() && !record.release_uncertain &&
         !record.release_marker_exhausted &&
         record.kind == osf::RecordKind::kPeriodic &&
         record.schema == osf::kProductSchemaV1 &&
         record.payload_size == tlp::kPeriodicPlaintextSize;
}
}  // namespace

void Kq05BackendReceiptReleaseOwner::clear() {
  wipe(wire_, sizeof(wire_));
  wire_size_ = 0U;
  index_ = 0U;
  active_ = false;
  waiting_flash_ = false;
}

Kq05BackendReceiptReleaseOwner::Submit
Kq05BackendReceiptReleaseOwner::submit(const uint8_t* frame, size_t length) {
  if (active_) return Submit::kBusy;
  AuthenticatedKq05BackendReceipt proof{};
  if (verifier_.verify(frame, length, records_, proof) !=
          Kq05BackendReceiptVerifier::Result::kAuthenticated ||
      !proof.authenticated_) return Submit::kAuthenticationRejected;

  // All-or-nothing preflight. An authentic MAC for a missing/foreign record
  // cannot initiate even a partial set of Flash mutations.
  if (records_.store().busy() || records_.pending())
    return Submit::kStorageUnavailable;
  for (uint8_t i = 0U; i < proof.header_.count; ++i) {
    osf::RecordIdentity id{};
    id.incarnation = proof.header_.incarnation;
    id.sequence = proof.header_.sequences[i];
    ObservationStore::Record record{};
    if (records_.store().lookup(id, record) !=
            ObservationStore::LookupResult::kFound ||
        !usable(record, id)) return Submit::kStorageUnavailable;
  }

  if (frame == nullptr || length > sizeof(wire_))
    return Submit::kAuthenticationRejected;
  memcpy(wire_, frame, length);
  wire_size_ = length;
  index_ = 0U;
  released_ = 0U;
  waiting_flash_ = false;
  active_ = true;
  return Submit::kStarted;
}

Kq05BackendReceiptReleaseOwner::Service
Kq05BackendReceiptReleaseOwner::service() {
  if (!active_) return Service::kIdle;

  if (waiting_flash_) {
    osf::RecordIdentity committed{};
    bool durable = false;
    if (!records_.takeCustodyReleaseOutcome(committed, durable))
      return Service::kWaitingFlash;
    AuthenticatedKq05BackendReceipt proof{};
    const auto verified = verifier_.verify(
        wire_, wire_size_, records_, proof);
    if (!durable ||
        verified != Kq05BackendReceiptVerifier::Result::kAuthenticated ||
        !proof.authenticated_ ||
        index_ >= proof.header_.count ||
        committed.incarnation != proof.header_.incarnation ||
        committed.sequence != proof.header_.sequences[index_]) {
      clear();
      return Service::kFailed;
    }
    ++released_;
    ++index_;
    waiting_flash_ = false;
  }

  AuthenticatedKq05BackendReceipt proof{};
  if (verifier_.verify(wire_, wire_size_, records_, proof) !=
          Kq05BackendReceiptVerifier::Result::kAuthenticated ||
      !proof.authenticated_) {
    clear();
    return Service::kFailed;
  }

  while (index_ < proof.header_.count) {
    osf::RecordIdentity id{};
    id.incarnation = proof.header_.incarnation;
    id.sequence = proof.header_.sequences[index_];

    if (records_.store().busy() || records_.pending() ||
        records_.preparing())
      return Service::kWaitingStorage;
    ObservationStore::Record record{};
    if (records_.store().lookup(id, record) !=
            ObservationStore::LookupResult::kFound ||
        !usable(record, id)) {
      clear();
      return Service::kFailed;
    }
    if (record.released) {
      ++index_;  // idempotent replay: never write a second marker
      continue;
    }
    // The only friend with this authority; Flash readback happens later in
    // ProductRecordStore::poll(), not here or in a radio callback.
    if (!records_.requestAuthenticatedBackendReceiptRelease(id)) {
      if (records_.store().busy() || records_.pending())
        return Service::kWaitingStorage;
      clear();
      return Service::kFailed;
    }
    waiting_flash_ = true;
    return Service::kWaitingFlash;
  }

  const uint8_t released = released_;
  clear();
  return released != 0U ? Service::kApplied : Service::kDuplicateOnly;
}

}  // namespace orun_tlp
