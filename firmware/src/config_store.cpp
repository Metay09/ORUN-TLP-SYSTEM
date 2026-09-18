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

bool validCandidate(const config_format::Config& candidate) {
  return candidate.tracking_interval_seconds > 0 &&
         candidate.tracking_interval_seconds <= gnss_config::kMaxTrackingIntervalSeconds;
}
}  // namespace

bool ConfigStore::begin() {
  ready_ = false;
  job_ = Job::kNone;
  diagnostics_ = {};
  active_page_ = -1;
  generation_ = 0;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
  save_result_ready_ = save_success_ = false;
  // Set the safe fallback before recovery even attempts to touch flash, so
  // a blank partition, a corrupt page, or a flash backend that fails begin()
  // all leave config() reporting the same 180s default as pre-M7P5 main.
  config_ = defaultConfig();
  if (!flash_.begin() || !recover()) return false;
  ready_ = true;
  return true;
}

bool ConfigStore::recover() {
  for (unsigned page = 0; page < kConfigPageCount; ++page) {
    uint8_t bytes[config_format::kRecordSize];
    if (!flash_.read(page * storage_config::kPageSize, bytes, sizeof(bytes))) return false;
    uint64_t candidate_generation = 0;
    config_format::Config candidate{};
    if (!config_format::decode(bytes, candidate_generation, candidate)) {
      if (!journal_format::erased(bytes, sizeof(bytes))) ++diagnostics_.recovery_corruptions;
      continue;
    }
    // A structurally sealed (correct magic/CRC/commit) record can still
    // carry a semantically invalid field -- e.g. a lower kMaxTrackingIntervalSeconds
    // in a later firmware than the one that wrote it. Never trust a
    // recovered candidate any less strictly than requestSave() would have.
    if (!validCandidate(candidate)) {
      ++diagnostics_.recovery_corruptions;
      continue;
    }
    if (candidate_generation > generation_) {
      generation_ = candidate_generation;
      active_page_ = static_cast<int>(page);
      config_ = candidate;
    }
  }
  return true;
}

bool ConfigStore::requestSave(const config_format::Config& candidate) {
  if (!ready_ || busy()) return false;
  if (!validCandidate(candidate)) {
    ++diagnostics_.rejected_candidates;
    return false;
  }
  if (candidate.tracking_interval_seconds == config_.tracking_interval_seconds &&
      candidate.battery_capacity_mah == config_.battery_capacity_mah) {
    ++diagnostics_.skipped_unchanged;
    return true;
  }
  // Fail closed rather than let a second async save silently overwrite the
  // first's still-unread result: without this, busy()==false the instant a
  // save finishes (poll() clears job_ in finishSave()/fail()), so a caller
  // could requestSave() again before ever calling takeSaveResult(), and a
  // later takeSaveResult() would then return the SECOND save's outcome to a
  // caller who believes it is still waiting on the first. Consuming the
  // prior result is the only way to unblock this.
  if (save_result_ready_) {
    ++diagnostics_.blocked_pending_result;
    return false;
  }
  return startSave(candidate);
}

bool ConfigStore::requestReset() { return requestSave(defaultConfig()); }

bool ConfigStore::startSave(const config_format::Config& candidate) {
  if (generation_ == UINT64_MAX) return false;  // exhausted; refuse rather than wrap.
  target_page_ = active_page_ < 0 ? 0 : (1 - static_cast<unsigned>(active_page_));
  pending_generation_ = generation_ + 1;
  pending_config_ = candidate;
  job_ = Job::kErase;
  blob_step_ = BlobStep::kBody;
  flash_op_awaiting_completion_ = false;
  return true;
}

FlashOpResult ConfigStore::writeBlob() {
  const uint32_t offset = target_page_ * storage_config::kPageSize;
  if (blob_step_ == BlobStep::kBody) {
    const FlashOpResult result = flash_op_awaiting_completion_
        ? flash_.pollPending()
        : flash_.program(offset, blob_, sizeof(blob_) - 4);
    if (result == FlashOpResult::kPending) {
      flash_op_awaiting_completion_ = true;
      return FlashOpResult::kPending;
    }
    flash_op_awaiting_completion_ = false;
    if (result == FlashOpResult::kFailed) {
      fail();
      return FlashOpResult::kFailed;
    }
    blob_step_ = BlobStep::kCommit;
  }
  if (blob_step_ == BlobStep::kCommit) {
    const FlashOpResult result = flash_op_awaiting_completion_
        ? flash_.pollPending()
        : flash_.program(offset + sizeof(blob_) - 4, blob_ + sizeof(blob_) - 4, 4);
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
  uint8_t verify[config_format::kRecordSize];
  if (!flash_.read(offset, verify, sizeof(blob_)) || memcmp(verify, blob_, sizeof(blob_)) != 0) {
    blob_step_ = BlobStep::kBody;
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
  // Deliberately does NOT clear ready_ or touch config_/active_page_/
  // generation_: a save always targets the inactive page, so a failure here
  // never wrote to the previously committed page. The last-good config
  // remains active and readable, matching "an invalid candidate must not
  // replace the previous valid config" for flash-level failures too.
}

void ConfigStore::finishSave() {
  active_page_ = static_cast<int>(target_page_);
  generation_ = pending_generation_;
  config_ = pending_config_;
  job_ = Job::kNone;
  ++diagnostics_.saves;
  save_result_ready_ = true;
  save_success_ = true;
}

void ConfigStore::poll() {
  if (!ready_ || job_ == Job::kNone) return;
  if (job_ == Job::kErase) {
    const FlashOpResult result = flash_op_awaiting_completion_
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
    config_format::encode(pending_config_, pending_generation_, blob_);
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
