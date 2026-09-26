#include "config_store.h"

#include <string.h>

#include "gnss_config.h"
#include "journal_format.h"
#include "storage_config.h"

namespace orun_tlp {
namespace {
constexpr unsigned kConfigPageCount = storage_config::kFutureConfigRegionPages;
static_assert(kConfigPageCount == 2, "A/B ping-pong assumes exactly two pages");

config_format::Config defaultConfig() {
  return config_format::Config{gnss_config::kTrackingIntervalSeconds, 0};
}

bool sameConfig(const config_format::Config& a,
                const config_format::Config& b) {
  return a.tracking_interval_seconds == b.tracking_interval_seconds &&
         a.battery_capacity_mah == b.battery_capacity_mah;
}

bool validCandidate(const config_format::Config& candidate) {
  return candidate.tracking_interval_seconds > 0 &&
         candidate.tracking_interval_seconds <=
             gnss_config::kMaxTrackingIntervalSeconds;
}

bool evidenceIsLegacy(config_format::PageEvidence evidence) {
  return evidence == config_format::PageEvidence::kLegacyV1Committed ||
         evidence ==
             config_format::PageEvidence::kLegacyV1UncommittedOrTorn ||
         evidence ==
             config_format::PageEvidence::kLegacyV1CommittedCorrupt;
}

bool evidenceIsSafeUncommitted(config_format::PageEvidence evidence) {
  return evidence == config_format::PageEvidence::kErased ||
         evidence == config_format::PageEvidence::kV2UncommittedOrTorn;
}

bool exactSuccessor(const config_format::PageInspection& committed,
                    const config_format::PageInspection& staged) {
  return committed.generation != UINT64_MAX &&
         committed.token.revision != UINT32_MAX &&
         staged.generation == committed.generation + 1 &&
         staged.token.incarnation == committed.token.incarnation &&
         staged.token.revision == committed.token.revision + 1;
}

}  // namespace

void ConfigStore::clearRecoveredRuntimeState() {
  config_ = defaultConfig();
  token_ = {};
  token_state_ = ConfigTokenState::kUnavailable;
  semantic_unambiguous_ = false;
  maintenance_reset_required_ = false;
  application_config_committed_ = false;
  generation_ = 0;
  active_page_ = -1;
}

void ConfigStore::setMaintenance(ConfigTokenState token_state) {
  config_ = defaultConfig();
  token_ = {};
  token_state_ = token_state;
  semantic_unambiguous_ = false;
  maintenance_reset_required_ = true;
  application_config_committed_ = false;
  generation_ = 0;
  active_page_ = -1;
  ++diagnostics_.maintenance_lockouts;
}

void ConfigStore::setMaintenanceFallback(
    const config_format::PageInspection& fallback,
    ConfigTokenState token_state) {
  config_ = fallback.config;
  token_ = fallback.token;  // evidence only; stateToken() hides it unless VALID.
  token_state_ = token_state;
  semantic_unambiguous_ = true;
  maintenance_reset_required_ = true;
  application_config_committed_ =
      fallback.token.revision > 1 ||
      !sameConfig(fallback.config, defaultConfig());
  generation_ = fallback.generation;
  active_page_ = -1;  // no cache-authoritative page while maintenance-locked
  ++diagnostics_.maintenance_lockouts;
}

bool ConfigStore::begin() {
  ready_ = false;
  job_ = Job::kNone;
  diagnostics_ = {};
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
  save_result_ready_ = false;
  save_success_ = false;
  mutation_unreconciled_ = false;
  recovery_pending_ = false;
  clearRecoveredRuntimeState();

  if (!flash_.begin()) return false;
  if (!recover()) return false;

  // Blank partition is the only automatic fresh-baseline case in the current
  // clean-cutover product. recover() leaves it as defaults/FALLBACK_ONLY.
  if (!maintenance_reset_required_ && active_page_ < 0 &&
      token_state_ == ConfigTokenState::kUnavailable) {
    if (incarnation_source_ != nullptr) {
      (void)establishFreshBaseline();
    }
  }

  ready_ = true;
  return true;
}

bool ConfigStore::recover() {
  clearRecoveredRuntimeState();

  RecoveredPage pages[kConfigPageCount]{};
  bool all_erased = true;
  bool any_legacy = false;
  bool any_unsupported = false;
  bool any_supported_corrupt = false;
  unsigned committed_count = 0;
  int committed_pages[kConfigPageCount] = {-1, -1};

  for (unsigned page = 0; page < kConfigPageCount; ++page) {
    uint8_t bytes[config_format::kV2PagePrefixSize];
    if (!flash_.read(page * storage_config::kPageSize, bytes,
                     sizeof(bytes)))
      return false;

    if (!config_format::inspectPagePrefix(
            bytes, sizeof(bytes), pages[page].inspection))
      return false;

    const auto evidence = pages[page].inspection.evidence;
    if (evidence != config_format::PageEvidence::kErased)
      all_erased = false;

    if (evidenceIsLegacy(evidence)) {
      any_legacy = true;
      ++diagnostics_.legacy_pages_seen;
      continue;
    }

    if (evidence == config_format::PageEvidence::kUnsupportedNewer) {
      any_unsupported = true;
      continue;
    }

    if (pages[page].inspection.has_decoded_record) {
      pages[page].semantic_valid =
          validCandidate(pages[page].inspection.config);
    }

    if (evidence == config_format::PageEvidence::kV2Committed) {
      if (!pages[page].semantic_valid) {
        ++diagnostics_.recovery_corruptions;
        any_supported_corrupt = true;
        continue;
      }
      committed_pages[committed_count++] = static_cast<int>(page);
      continue;
    }

    if (evidence == config_format::PageEvidence::kV2Staged) {
      // Structurally valid but semantically invalid stage was never active and
      // is equivalent to uncommitted/torn evidence for authority purposes.
      if (!pages[page].semantic_valid)
        ++diagnostics_.recovery_corruptions;
      continue;
    }

    if (evidence == config_format::PageEvidence::kErased ||
        evidence == config_format::PageEvidence::kV2UncommittedOrTorn)
      continue;

    // Retired committed, partial commit, committed corruption and generic
    // supported corruption all invalidate cache-authoritative token state in
    // this first runtime slice. Re-baseline is deliberately deferred.
    ++diagnostics_.recovery_corruptions;
    any_supported_corrupt = true;
  }

  if (all_erased) return true;

  // Clean-cutover boundary: never adopt or rewrite legacy development state.
  if (any_legacy) {
    setMaintenance(any_unsupported || any_supported_corrupt ||
                           committed_count > 0
                       ? ConfigTokenState::kUncertain
                       : ConfigTokenState::kUnavailable);
    return true;
  }

  if (any_unsupported) {
    setMaintenance(ConfigTokenState::kUncertain);
    return true;
  }

  if (committed_count == 0) {
    // Non-erased v2 evidence without a normal committed authority requires
    // maintenance/re-baseline in a later slice. Do not promote a stage or
    // partial commit after reboot.
    setMaintenance(any_supported_corrupt
                       ? ConfigTokenState::kUncertain
                       : ConfigTokenState::kUnavailable);
    return true;
  }

  if (committed_count == 2) {
    const int a = committed_pages[0];
    const int b = committed_pages[1];
    const auto& pa = pages[a].inspection;
    const auto& pb = pages[b].inspection;

    int high = a;
    int low = b;
    if (pb.generation > pa.generation) {
      high = b;
      low = a;
    }

    const auto& hi = pages[high].inspection;
    const auto& lo = pages[low].inspection;

    if (lo.generation == UINT64_MAX ||
        hi.generation != lo.generation + 1 ||
        hi.token.incarnation != lo.token.incarnation ||
        lo.token.revision == UINT32_MAX ||
        hi.token.revision != lo.token.revision + 1) {
      if (sameConfig(hi.config, lo.config))
        setMaintenanceFallback(hi, ConfigTokenState::kUncertain);
      else
        setMaintenance(ConfigTokenState::kUncertain);
      return true;
    }

    active_page_ = high;
    generation_ = hi.generation;
    config_ = hi.config;
    token_ = hi.token;
    application_config_committed_ =
        hi.token.revision > 1 || !sameConfig(hi.config, defaultConfig());
    token_state_ = ConfigTokenState::kValid;
    semantic_unambiguous_ = true;
    return true;
  }

  const int committed_page = committed_pages[0];
  const int other_page = 1 - committed_page;
  const auto& committed = pages[committed_page].inspection;
  const auto& other = pages[other_page].inspection;

  if (any_supported_corrupt) {
    // The committed semantic copy is still useful to the device/user, but the
    // contradictory inactive-page evidence makes its token unsafe for CAS.
    setMaintenanceFallback(committed, ConfigTokenState::kUncertain);
    return true;
  }

  if (other.evidence == config_format::PageEvidence::kV2Staged) {
    if (!pages[other_page].semantic_valid) {
      // A never-committed semantically invalid stage is equivalent to
      // uncommitted/torn evidence; it cannot supersede the committed page.
    } else if (!exactSuccessor(committed, other)) {
      setMaintenanceFallback(committed, ConfigTokenState::kUncertain);
      return true;
    }
  } else if (!evidenceIsSafeUncommitted(other.evidence)) {
    setMaintenance(ConfigTokenState::kUncertain);
    return true;
  }

  active_page_ = committed_page;
  generation_ = committed.generation;
  config_ = committed.config;
  token_ = committed.token;
  application_config_committed_ =
      committed.token.revision > 1 ||
      !sameConfig(committed.config, defaultConfig());
  token_state_ = ConfigTokenState::kValid;
  semantic_unambiguous_ = true;
  return true;
}

bool ConfigStore::writeFreshBaseline(
    const config_format::V2Record& record) {
  uint8_t bytes[config_format::kV2RecordSize];
  config_format::encodeV2(record, bytes);

  const uint32_t offset = 0;
  const FlashOpResult body =
      flash_.program(offset, bytes, config_format::kV2BodyAndCrcSize);
  if (body != FlashOpResult::kDone) return false;

  uint8_t stage_verify[config_format::kV2BodyAndCrcSize];
  if (!flash_.read(offset, stage_verify, sizeof(stage_verify)) ||
      memcmp(stage_verify, bytes, sizeof(stage_verify)) != 0)
    return false;

  const FlashOpResult commit =
      flash_.program(offset + config_format::kV2CommitOffset,
                     bytes + config_format::kV2CommitOffset, 4);
  if (commit != FlashOpResult::kDone) return false;

  uint8_t verify[config_format::kV2RecordSize];
  if (!flash_.read(offset, verify, sizeof(verify)) ||
      memcmp(verify, bytes, sizeof(verify)) != 0)
    return false;

  return true;
}

bool ConfigStore::establishFreshBaseline() {
  if (incarnation_source_ == nullptr) return false;

  uint64_t incarnation = 0;
  if (!incarnation_source_->generate(incarnation) || incarnation == 0)
    return false;

  const config_format::V2Record record(
      1, defaultConfig(),
      config_format::StateToken(incarnation, 1));

  if (!writeFreshBaseline(record)) {
    ++diagnostics_.baseline_failures;
    if (flash_.hasUnreconciledMutation()) {
      mutation_unreconciled_ = true;
      token_state_ = ConfigTokenState::kUncertain;
      ++diagnostics_.unreconciled_mutation_faults;
    } else if (!recover()) {
      ready_ = false;
    }
    return false;
  }

  active_page_ = 0;
  generation_ = 1;
  config_ = record.config;
  token_ = record.token;
  token_state_ = ConfigTokenState::kValid;
  semantic_unambiguous_ = true;
  maintenance_reset_required_ = false;
  application_config_committed_ = false;
  ++diagnostics_.baseline_commits;
  return true;
}

bool ConfigStore::requestSave(const config_format::Config& candidate) {
  if (!ready_ || busy() || mutation_unreconciled_ || recovery_pending_)
    return false;

  if (!validCandidate(candidate)) {
    ++diagnostics_.rejected_candidates;
    return false;
  }

  if (sameConfig(candidate, config_)) {
    if (!semantic_unambiguous_) return false;
    ++diagnostics_.skipped_unchanged;
    return true;
  }

  if (maintenance_reset_required_ ||
      token_state_ != ConfigTokenState::kValid ||
      active_page_ < 0) {
    ++diagnostics_.maintenance_lockouts;
    return false;
  }

  if (save_result_ready_) {
    ++diagnostics_.blocked_pending_result;
    return false;
  }

  return startSave(candidate);
}

bool ConfigStore::requestReset() { return requestSave(defaultConfig()); }

bool ConfigStore::startSave(const config_format::Config& candidate) {
  if (generation_ == UINT64_MAX || token_.revision == UINT32_MAX)
    return false;

  target_page_ = 1U - static_cast<unsigned>(active_page_);
  pending_generation_ = generation_ + 1;
  pending_config_ = candidate;
  pending_token_ =
      config_format::StateToken(token_.incarnation, token_.revision + 1);

  const config_format::V2Record record(
      pending_generation_, pending_config_, pending_token_);
  config_format::encodeV2(record, blob_);

  job_ = Job::kErase;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
  return true;
}

FlashOpResult ConfigStore::writeBlob() {
  const uint32_t offset = target_page_ * storage_config::kPageSize;

  if (blob_step_ == BlobStep::kBody) {
    const FlashOpResult result =
        flash_op_awaiting_completion_
            ? flash_.pollPending()
            : flash_.program(offset, blob_,
                             config_format::kV2BodyAndCrcSize);
    if (result == FlashOpResult::kPending) {
      flash_op_awaiting_completion_ = true;
      return FlashOpResult::kPending;
    }

    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) {
      fail();
      return FlashOpResult::kFailed;
    }

    uint8_t verify[config_format::kV2BodyAndCrcSize];
    if (!flash_.read(offset, verify, sizeof(verify)) ||
        memcmp(verify, blob_, sizeof(verify)) != 0) {
      fail();
      return FlashOpResult::kFailed;
    }

    blob_step_ = BlobStep::kCommit;
  }

  if (blob_step_ == BlobStep::kCommit) {
    const FlashOpResult result =
        flash_op_awaiting_completion_
            ? flash_.pollPending()
            : flash_.program(offset + config_format::kV2CommitOffset,
                             blob_ + config_format::kV2CommitOffset, 4);
    if (result == FlashOpResult::kPending) {
      flash_op_awaiting_completion_ = true;
      return FlashOpResult::kPending;
    }

    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) {
      fail();
      return FlashOpResult::kFailed;
    }

    blob_step_ = BlobStep::kVerify;
  }

  uint8_t verify[config_format::kV2RecordSize];
  if (!flash_.read(offset, verify, sizeof(verify)) ||
      memcmp(verify, blob_, sizeof(verify)) != 0) {
    fail();
    return FlashOpResult::kFailed;
  }

  blob_step_ = BlobStep::kBody;
  return FlashOpResult::kDone;
}

void ConfigStore::fail() {
  ++diagnostics_.save_failures;
  save_result_ready_ = true;
  save_success_ = false;
  job_ = Job::kNone;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;

  // A failed target-page erase/program can leave local evidence that makes the
  // formerly valid token unsafe to cache, even though the previous semantic
  // config remains the best runtime fallback.
  token_state_ = ConfigTokenState::kUncertain;
  semantic_unambiguous_ = true;
  recovery_pending_ = true;

  if (flash_.hasUnreconciledMutation()) {
    mutation_unreconciled_ = true;
    ++diagnostics_.unreconciled_mutation_faults;
  }
}

void ConfigStore::finishSave() {
  active_page_ = static_cast<int>(target_page_);
  generation_ = pending_generation_;
  config_ = pending_config_;
  token_ = pending_token_;
  token_state_ = ConfigTokenState::kValid;
  semantic_unambiguous_ = true;
  maintenance_reset_required_ = false;
  application_config_committed_ = true;
  job_ = Job::kNone;
  ++diagnostics_.saves;
  save_result_ready_ = true;
  save_success_ = true;
}

void ConfigStore::poll() {
  if (!ready_) return;

  if (mutation_unreconciled_) {
    if (flash_.hasUnreconciledMutation()) return;
    mutation_unreconciled_ = false;
    recovery_pending_ = true;
  }

  if (job_ == Job::kNone) {
    if (recovery_pending_) {
      recovery_pending_ = false;
      if (!recover()) {
        ready_ = false;
        return;
      }
      ++diagnostics_.recovery_reconciliations;
    }
    return;
  }

  if (job_ == Job::kErase) {
    const FlashOpResult result =
        flash_op_awaiting_completion_
            ? flash_.pollPending()
            : flash_.erasePage(target_page_);
    if (result == FlashOpResult::kPending) {
      flash_op_awaiting_completion_ = true;
      return;
    }

    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) {
      fail();
      return;
    }

    job_ = Job::kWrite;
    return;
  }

  if (writeBlob() != FlashOpResult::kDone) return;
  finishSave();
}

bool ConfigStore::takeSaveResult(bool& success) {
  if (!save_result_ready_) return false;
  success = save_success_;
  save_result_ready_ = false;
  return true;
}

}  // namespace orun_tlp
