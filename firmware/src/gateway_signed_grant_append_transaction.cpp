#include "gateway_signed_grant_append_transaction.h"

#if defined(ORUN_SF5K3AC_TEST_ONLY_TRANSACTION)
#include <string.h>

namespace orun_tlp {
namespace {
namespace fmt = gateway_grant_format;

void secureWipe(void* dest, size_t n) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(dest);
  while (n-- != 0U) *p++ = 0U;
}
uint32_t getBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24U) | (uint32_t(p[1]) << 16U) |
         (uint32_t(p[2]) << 8U) | uint32_t(p[3]);
}
}  // namespace

GatewaySignedGrantAppendTransaction::~GatewaySignedGrantAppendTransaction() {
  wipe();
}

void GatewaySignedGrantAppendTransaction::wipe() {
  secureWipe(source_snapshot_, sizeof(source_snapshot_));
  secureWipe(candidate_, sizeof(candidate_));
}
void GatewaySignedGrantAppendTransaction::fault() {
  wipe();
  waiting_ = false;
  state_ = State::kFault;
}

bool GatewaySignedGrantAppendTransaction::exactRead(
    uint32_t offset, const uint8_t* expected, size_t length) const {
  if (expected == nullptr) return false;
  uint8_t block[64]{};
  for (size_t n = 0U; n < length; n += sizeof(block)) {
    const size_t count = length - n < sizeof(block)
                             ? length - n : sizeof(block);
    if (!flash_.read(offset + static_cast<uint32_t>(n), block, count) ||
        memcmp(block, expected + n, count) != 0)
      return false;
  }
  return true;
}

bool GatewaySignedGrantAppendTransaction::erasedPage(uint16_t page) const {
  const uint32_t base = static_cast<uint32_t>(page) * fmt::kPageBytes;
  const uint8_t erased[64] = {
      0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
      0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
      0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
      0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
      0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
      0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
      0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
      0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
  };
  uint8_t raw[64]{};
  for (uint32_t offset = 0U; offset < fmt::kPageBytes;
       offset += sizeof(raw)) {
    if (!flash_.read(base + offset, raw, sizeof(raw)) ||
        memcmp(raw, erased, sizeof(erased)) != 0)
      return false;
  }
  return true;
}

bool GatewaySignedGrantAppendTransaction::start(
    GatewayGrantSnapshotReader& current,
    const GatewayGrantSnapshotReader& proposed,
    const GatewayGrantRevisionAnchor& anchor, uint64_t gateway,
    bool exclusive, bool radio_quiesced) {
  if (state_ != State::kIdle || !exclusive || !radio_quiesced ||
      flash_.hasUnreconciledMutation() ||
      &current.flash_ != &flash_ ||
      &current.verifier_ != &proposed.verifier_ ||
      current.active_page_ >= fmt::kPageCount)
    return false;
  if (GatewaySignedGrantAppendPreflight::inspect(
          current, proposed, anchor, gateway) !=
      GatewaySignedGrantAppendPreflight::Verdict::
          kEligibleForSeparateCommitReview)
    return false;

  source_ = current.active_page_;
  destination_ = static_cast<uint16_t>(1U - source_);
  current_revision_ = current.active_generation_;
  gateway_ = gateway;
  anchor_ = &anchor;
  issuer_ = &current.verifier_;
  memcpy(source_snapshot_, current.scratch_, sizeof(source_snapshot_));
  memcpy(candidate_, proposed.scratch_, sizeof(candidate_));

  if (getBe32(source_snapshot_ + fmt::kCommitOffset) != 0U ||
      getBe32(candidate_ + fmt::kCommitOffset) != 0U ||
      !exactRead(static_cast<uint32_t>(source_) * fmt::kPageBytes,
                 source_snapshot_, sizeof(source_snapshot_)) ||
      GatewaySignedGrantAppendPreflight::inspect(
          current, proposed, anchor, gateway) !=
      GatewaySignedGrantAppendPreflight::Verdict::
          kEligibleForSeparateCommitReview) {
    wipe();
    return false;
  }

  // No more live ACK can be issued using an out-of-date authorization
  // while its replacement is being committed. This stays latched even if
  // later flash mutations fail.
  current.denyForCurrentBoot();
  phase_ = Phase::kErase;
  waiting_ = false;
  offset_ = 0U;
  current_length_ = 0U;
  state_ = State::kBusy;
  return true;
}

bool GatewaySignedGrantAppendTransaction::verifyPreCommit() const {
  if (flash_.hasUnreconciledMutation() ||
      anchor_ == nullptr || issuer_ == nullptr) return false;
  uint64_t anchor_revision = 0U;
  if (!anchor_->currentRevision(gateway_, anchor_revision) ||
      anchor_revision != current_revision_) return false;

  if (!exactRead(static_cast<uint32_t>(source_) * fmt::kPageBytes,
                 source_snapshot_, sizeof(source_snapshot_)) ||
      !exactRead(static_cast<uint32_t>(destination_) * fmt::kPageBytes,
                 candidate_, fmt::kCommitOffset))
    return false;

  // Commit MUST still be erased, and all unallocated tail bytes remain FF.
  uint8_t expected[64]{};
  memset(expected, 0xFF, sizeof(expected));
  const uint32_t tail_base =
      uint32_t(destination_) * fmt::kPageBytes + fmt::kCommitOffset;
  const uint32_t tail_size = fmt::kPageBytes - fmt::kCommitOffset;
  for (uint32_t offset = 0U; offset < tail_size; offset += sizeof(expected)) {
    const size_t n = tail_size - offset < sizeof(expected)
                         ? tail_size - offset : sizeof(expected);
    if (!exactRead(tail_base + offset, expected, n))
      return false;
  }

  uint8_t proof[fmt::kProofBytes]{};
  memcpy(proof, candidate_ + fmt::kPayloadBytes, sizeof(proof));
  const bool valid = issuer_->verify(candidate_, fmt::kPayloadBytes, proof);
  secureWipe(proof, sizeof(proof));
  return valid;
}

GatewaySignedGrantAppendTransaction::State
GatewaySignedGrantAppendTransaction::poll() {
  if (state_ != State::kBusy) return state_;
  if (flash_.hasUnreconciledMutation()) {
    fault();
    return state_;
  }

  const uint32_t dest_base = uint32_t(destination_) * fmt::kPageBytes;
  if (phase_ == Phase::kPreCommit) {
    if (!verifyPreCommit()) {
      fault();
      return state_;
    }
    phase_ = Phase::kCommit;
    return state_;
  }

  // The final commit is a DISTINCT poll pass. Recheck the independent
  // anchor and signed bytes at the last safe point before programming its
  // irreversible zero marker. After a pending completion, an unknown
  // outcome is never automatically retried.
  if (phase_ == Phase::kCommit && !waiting_ &&
      !verifyPreCommit()) {
    fault();
    return state_;
  }

  const uint32_t remaining = phase_ == Phase::kBody
      ? fmt::kCommitOffset - offset_ : 0U;
  if (phase_ == Phase::kBody && remaining == 0U) {
    phase_ = Phase::kPreCommit;
    return state_;
  }
  if (phase_ == Phase::kBody) {
    current_length_ = remaining < 64U ? remaining : 64U;
    if (current_length_ == 0U || (current_length_ & 3U) != 0U) {
      fault();
      return state_;
    }
  }
  const uint8_t commit[4]{};
  const FlashOpResult operation = waiting_ ? flash_.pollPending() :
      phase_ == Phase::kErase ? flash_.erasePage(destination_) :
      phase_ == Phase::kBody ?
          flash_.program(dest_base + offset_, candidate_ + offset_,
                         current_length_) :
          flash_.program(dest_base + fmt::kCommitOffset, commit,
                         sizeof(commit));
  if (operation == FlashOpResult::kPending) {
    waiting_ = true;
    return state_;
  }
  if (operation != FlashOpResult::kDone) {
    fault();
    return state_;
  }
  waiting_ = false;

  if (phase_ == Phase::kErase) {
    if (!erasedPage(destination_)) fault();
    else phase_ = Phase::kBody;
    return state_;
  }
  if (phase_ == Phase::kBody) {
    if (!exactRead(dest_base + offset_, candidate_ + offset_,
                   current_length_)) {
      fault();
      return state_;
    }
    offset_ += current_length_;
    return state_;
  }

  if (!exactRead(dest_base, candidate_, sizeof(candidate_)) ||
      !exactRead(uint32_t(source_) * fmt::kPageBytes, source_snapshot_,
                 sizeof(source_snapshot_))) {
    fault();
    return state_;
  }
  // A committed page is never published if the independent authority
  // changed during the final physical flash operation. It can be recovered
  // only by a separately reviewed, authenticated enrollment reconciliation.
  uint64_t after_commit_revision = 0U;
  if (anchor_ == nullptr ||
      !anchor_->currentRevision(gateway_, after_commit_revision) ||
      after_commit_revision != current_revision_) {
    fault();
    return state_;
  }

  // COMMITTED != AUTHORIZED. Until the independent monotonic anchor
  // advances to the signed new revision, the freshest signed page and
  // checkpoint intentionally disagree. A fresh boot must FAIL CLOSED.
  wipe();
  state_ = State::kCommittedAwaitingAnchor;
  return state_;
}

}  // namespace orun_tlp
#endif
