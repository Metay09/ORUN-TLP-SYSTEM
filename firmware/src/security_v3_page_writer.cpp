#include "security_v3_page_writer.h"

#include <string.h>
#include "security_v3_ab_preflight.h"
#include "journal_format.h"
#include "storage_config.h"

namespace orun_tlp {
namespace security_v3_page_writer {
namespace {
namespace sf = security_format;
namespace v3 = security_v3_format;
namespace ab = security_v3_ab_preflight;
namespace jf = journal_format;
constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kCount = storage_config::kFutureSecurityRegionPages;
static_assert(kPageSize == v3::kPageBytes && kCount == 2U,
              "v3 writer must stay in its reserved two-page partition");

class FlashJournalReader : public v3::JournalReader {
 public:
  FlashJournalReader(const FlashBackend& backend, uint8_t page)
      : flash_(backend), base_(static_cast<uint32_t>(page) * kPageSize +
                                static_cast<uint32_t>(v3::kJournalOffset)) {}
  bool read(size_t offset, uint8_t* out, size_t size) const override {
    if (offset > v3::kJournalBytes ||
        size > v3::kJournalBytes - offset) return false;
    return flash_.read(base_ + static_cast<uint32_t>(offset), out, size);
  }
 private:
  const FlashBackend& flash_;
  const uint32_t base_;
};

bool sameSnapshot(const v3::Snapshot& a, const v3::Snapshot& b) {
  if (memcmp(a.credential_id, b.credential_id,
             sf::kCredentialIdSize) != 0 ||
      a.key_epoch != b.key_epoch || a.tx_bound != b.tx_bound ||
      a.a2d_bound != b.a2d_bound ||
      a.custody_floor != b.custody_floor ||
      a.policy_revision != b.policy_revision)
    return false;
  for (unsigned slot = 0U; slot < v3::kGatewayCount; ++slot) {
    if (a.gateways[slot].gateway_id != b.gateways[slot].gateway_id ||
        a.gateways[slot].generation != b.gateways[slot].generation)
      return false;
  }
  return true;
}

bool sameCredential(const sf::Credential& a,
                    const sf::Credential& b) {
  return memcmp(a.credential_id, b.credential_id,
                sf::kCredentialIdSize) == 0 &&
         memcmp(a.k_root, b.k_root, sf::kKRootSize) == 0 &&
         a.key_epoch == b.key_epoch &&
         a.device_identity == b.device_identity;
}

bool completeErasedPage(const FlashBackend& flash, uint8_t page) {
  uint8_t raw[68]{};
  const uint32_t base = static_cast<uint32_t>(page) * kPageSize;
  for (uint32_t offset = 0U; offset < kPageSize;
       offset += static_cast<uint32_t>(sizeof(raw))) {
    const uint32_t remaining = kPageSize - offset;
    const size_t length = remaining < sizeof(raw) ?
                          remaining : sizeof(raw);
    if (!flash.read(base + offset, raw, length) ||
        !jf::erased(raw, length)) return false;
  }
  return true;
}
}  // namespace

bool SnapshotTransaction::compareSourceSnapshot(
    const v3::Snapshot& source,
    const v3::Snapshot& requested) const {
  // Without an authenticated policy mutation owner, this writer can only
  // copy exactly the previously COMMITTED authority. No new grants, no
  // counter invention, no reset of tombstones, no credential rotation.
  return sameSnapshot(source, requested);
}

bool SnapshotTransaction::snapshotFromSource(
    uint8_t page, v3::Snapshot& out) const {
  FlashJournalReader reader(flash_, page);
  return v3::recoverCommittedJournalStream(
      reader, credential_.credential_id, credential_.key_epoch, out);
}

bool SnapshotTransaction::start(
    uint8_t destination_page,
    const sf::Credential& credential,
    const v3::Snapshot& preserved) {
  if (status_ != Status::kIdle || destination_page >= kCount ||
      credential.device_identity == 0U ||
      credential.key_epoch == UINT32_MAX)
    return false;
  uint8_t encoded_prefix[v3::kSnapshotCount][v3::kRecordBytes]{};
  if (!v3::encodeSnapshotPrefix(preserved, encoded_prefix) ||
      memcmp(preserved.credential_id, credential.credential_id,
             sf::kCredentialIdSize) != 0 ||
      preserved.key_epoch != credential.key_epoch)
    return false;

  ab::Summary summary{};
  const ab::Result selection =
      ab::inspect(flash_, credential.device_identity, summary);
  if ((selection != ab::Result::kV2 &&
       selection != ab::Result::kV3) ||
      summary.selected_page != (1U - destination_page) ||
      summary.generation == UINT64_MAX)
    return false;

  // The new page generation is always computed from the actual physical
  // source election, NEVER supplied by a radio/admin caller.
  uint8_t src_bytes[sf::kCredentialRecordSize]{};
  const uint32_t source_offset =
      (1U - static_cast<uint32_t>(destination_page)) * kPageSize +
      sf::kPageHeaderSize;
  sf::Credential source_credential{};
  if (!flash_.read(source_offset, src_bytes, sizeof(src_bytes)) ||
      !sf::decodeCredential(src_bytes, source_credential) ||
      !sameCredential(source_credential, credential))
    return false;

  v3::Snapshot from_source{};
  if (selection == ab::Result::kV3) {
    credential_ = source_credential; // temporary; no secret exported
    if (!snapshotFromSource(summary.selected_page, from_source)) {
      credential_ = sf::Credential{};
      return false;
    }
  } else {
    memcpy(from_source.credential_id, credential.credential_id,
           sf::kCredentialIdSize);
    from_source.key_epoch = credential.key_epoch;
    from_source.tx_bound = summary.tx_reserved_bound;
    from_source.a2d_bound = summary.a2d_replay_bound;
    // A v2 security page has no authorized Gateway policy state.
  }
  if (!compareSourceSnapshot(from_source, preserved)) {
    credential_ = sf::Credential{};
    return false;
  }

  credential_ = source_credential;
  snapshot_ = preserved;
  dest_ = destination_page;
  generation_ = summary.generation + 1U;
  record_total_ = static_cast<uint32_t>(v3::kSnapshotCount) +
                  (snapshot_.tx_bound != 0U ? 1U : 0U) +
                  (snapshot_.a2d_bound != 0U ? 1U : 0U);
  record_index_ = 0U;
  completed_ops_ = 0U;
  step_ = Step::kBody;
  awaiting_ = false;
  phase_ = Phase::kErase;
  status_ = Status::kBusy;
  return true;
}

void SnapshotTransaction::fault() {
  awaiting_ = false;
  status_ = Status::kFault;
  credential_ = sf::Credential{};
  snapshot_ = v3::Snapshot{};
  memset(blob_, 0, sizeof(blob_));
}

bool SnapshotTransaction::verifiedRead(
    uint32_t offset, const uint8_t* expected, size_t size) const {
  uint8_t raw[sf::kCredentialRecordSize]{};
  if (size > sizeof(raw) ||
      !flash_.read(offset, raw, size)) return false;
  return memcmp(raw, expected, size) == 0;
}

FlashOpResult SnapshotTransaction::executeProgram(
    uint32_t offset, const uint8_t* data, uint32_t size) {
  const FlashOpResult r = awaiting_ ? flash_.pollPending() :
                                     flash_.program(offset, data, size);
  if (r == FlashOpResult::kPending) {
    awaiting_ = true;
    return r;
  }
  awaiting_ = false;
  if (r != FlashOpResult::kDone ||
      flash_.hasUnreconciledMutation() ||
      !verifiedRead(offset, data, size)) {
    fault();
    return FlashOpResult::kFailed;
  }
  ++completed_ops_;
  return FlashOpResult::kDone;
}

bool SnapshotTransaction::buildRecord(uint32_t index) {
  uint8_t raw[v3::kRecordBytes]{};
  if (index < v3::kSnapshotCount) {
    uint8_t prefix[v3::kSnapshotCount][v3::kRecordBytes]{};
    if (!v3::encodeSnapshotPrefix(snapshot_, prefix)) return false;
    memcpy(raw, prefix[index], sizeof(raw));
  } else {
    const uint32_t after_prefix =
        index - static_cast<uint32_t>(v3::kSnapshotCount);
    const bool tx = snapshot_.tx_bound != 0U && after_prefix == 0U;
    const bool a2d =
        snapshot_.a2d_bound != 0U &&
        after_prefix == (snapshot_.tx_bound != 0U ? 1U : 0U);
    if (!tx && !a2d) return false;
    v3::Entry record{};
    memcpy(record.credential_id, credential_.credential_id,
           sf::kCredentialIdSize);
    record.key_epoch = credential_.key_epoch;
    record.kind = tx ? v3::Kind::kTxReserve :
                       v3::Kind::kA2dReplayReserve;
    record.value = tx ? snapshot_.tx_bound : snapshot_.a2d_bound;
    if (!v3::encodeRecord(record, raw)) return false;
  }
  memcpy(blob_, raw, sizeof(raw));
  blob_size_ = sizeof(raw);
  blob_offset_ = static_cast<uint32_t>(dest_) * kPageSize +
                 static_cast<uint32_t>(v3::kJournalOffset) +
                 index * static_cast<uint32_t>(v3::kRecordBytes);
  return true;
}

void SnapshotTransaction::nextRecord() {
  ++record_index_;
  step_ = Step::kBody;
  if (record_index_ == record_total_) {
    phase_ = Phase::kPreactivationCheck;
  } else if (!buildRecord(record_index_)) {
    fault();
  }
}

bool SnapshotTransaction::verifyBeforeActivate() const {
  // Every v3 snapshot and counter journal byte is checked again BEFORE
  // writing the 4-byte header activation marker. All unprogrammed
  // journal slots and the reserved tail must still be erased.
  const uint32_t base = static_cast<uint32_t>(dest_) * kPageSize;
  uint8_t header[sf::kPageHeaderSize]{};
  uint8_t credential[sf::kCredentialRecordSize]{};
  if (!flash_.read(base, header, sizeof(header)) ||
      !flash_.read(base + sf::kPageHeaderSize,
                   credential, sizeof(credential)) ||
      !jf::erased(header + 28U, 4U))
    return false;
  uint8_t expected_header[sf::kPageHeaderSize]{};
  sf::encodePageHeaderVersion(
      sf::PageHeader{generation_, credential_.device_identity},
      v3::kVersion, expected_header);
  if (memcmp(header, expected_header, 28U) != 0) return false;
  sf::Credential actual{};
  if (!sf::decodeCredential(credential, actual) ||
      !sameCredential(actual, credential_)) return false;
  const FlashJournalReader reader(flash_, dest_);
  v3::Snapshot recovered{};
  return v3::recoverCommittedJournalStream(
             reader, credential_.credential_id,
             credential_.key_epoch, recovered) &&
         sameSnapshot(snapshot_, recovered);
}

bool SnapshotTransaction::verifyCommittedHeader() const {
  const uint32_t base = static_cast<uint32_t>(dest_) * kPageSize;
  uint8_t header[sf::kPageHeaderSize]{};
  if (!flash_.read(base, header, sizeof(header)) ||
      jf::get32(header) != sf::kMagic ||
      header[4] != v3::kVersion ||
      header[5] != 0U || header[6] != 0U || header[7] != 0U ||
      jf::get64(header + 8U) != generation_ ||
      jf::get64(header + 16U) != credential_.device_identity ||
      jf::get32(header + 24U) != jf::crc32(header, 24U) ||
      jf::get32(header + 28U) != 0U)
    return false;
  return true;
}

Status SnapshotTransaction::poll() {
  if (status_ != Status::kBusy) return status_;
  const uint32_t base = static_cast<uint32_t>(dest_) * kPageSize;

  if (phase_ == Phase::kErase) {
    const FlashOpResult result = awaiting_ ? flash_.pollPending() :
                                            flash_.erasePage(dest_);
    if (result == FlashOpResult::kPending) {
      awaiting_ = true;
      return status_;
    }
    awaiting_ = false;
    if (result != FlashOpResult::kDone ||
        flash_.hasUnreconciledMutation() ||
        !completeErasedPage(flash_, dest_)) {
      fault();
      return status_;
    }
    ++completed_ops_;
    phase_ = Phase::kHeaderBody;
    uint8_t raw[sf::kPageHeaderSize]{};
    sf::encodePageHeaderVersion(
        sf::PageHeader{generation_, credential_.device_identity},
        v3::kVersion, raw);
    memcpy(blob_, raw, sizeof(raw));
    blob_size_ = sizeof(raw);
    blob_offset_ = base;
    return status_;
  }

  if (phase_ == Phase::kHeaderBody) {
    if (executeProgram(blob_offset_, blob_, blob_size_ - 4U) !=
        FlashOpResult::kDone) return status_;
    phase_ = Phase::kCredential;
    sf::encodeCredential(credential_, blob_);
    blob_size_ = sf::kCredentialRecordSize;
    blob_offset_ = base + sf::kPageHeaderSize;
    step_ = Step::kBody;
    return status_;
  }

  if (phase_ == Phase::kCredential ||
      phase_ == Phase::kRecords) {
    const uint32_t at = step_ == Step::kBody ?
                        blob_offset_ :
                        blob_offset_ + blob_size_ - 4U;
    const uint8_t* data = step_ == Step::kBody ?
                          blob_ : blob_ + blob_size_ - 4U;
    const uint32_t size = step_ == Step::kBody ?
                          blob_size_ - 4U : 4U;
    if (executeProgram(at, data, size) !=
        FlashOpResult::kDone) return status_;
    if (step_ == Step::kBody) {
      step_ = Step::kCommit;
      return status_;
    }
    // Individual record is committed only AFTER its body is persistent.
    if (!verifiedRead(blob_offset_, blob_, blob_size_)) {
      fault();
      return status_;
    }
    step_ = Step::kBody;
    if (phase_ == Phase::kCredential) {
      phase_ = Phase::kRecords;
      if (!buildRecord(record_index_)) fault();
    } else {
      nextRecord();
    }
    return status_;
  }

  if (phase_ == Phase::kPreactivationCheck) {
    if (!verifyBeforeActivate()) {
      fault();
      return status_;
    }
    phase_ = Phase::kActivate;
    return status_;
  }

  if (phase_ == Phase::kActivate) {
    const uint8_t commit[4]{};
    if (executeProgram(base + 28U, commit, sizeof(commit)) !=
        FlashOpResult::kDone) return status_;
    if (!verifyCommittedHeader()) {
      fault();
      return status_;
    }
    phase_ = Phase::kFinished;
    status_ = Status::kCommitted;
    // A COMPLETED physical page is not application authorization.
    credential_ = sf::Credential{};
    snapshot_ = v3::Snapshot{};
    memset(blob_, 0, sizeof(blob_));
  }
  return status_;
}

}  // namespace security_v3_page_writer
}  // namespace orun_tlp
