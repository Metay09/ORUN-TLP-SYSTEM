#pragma once
#include "flash_backend.h"
#include "journal_format.h"
#include "sequence_source.h"

namespace orun_tlp {
class HistoryStore : public SequenceSource {
 public:
  using Record = journal_format::Record;
  struct Diagnostics { uint32_t appended=0, append_failures=0, recovery_corruptions=0, overwritten=0, metadata_failures=0; };
  explicit HistoryStore(FlashBackend& backend) : flash_(backend) {}
  bool begin(uint64_t device_id);
  void poll();  // One synchronous journal step per call.
  bool ready() const { return ready_; }
  bool busy() const { return job_ != Job::kNone; }
  // Eligibility for a NEW fix/ticket; append() accepts an already issued ticket.
  bool canAppend() const { return appendIdle() && next_ticket_ < sequence_end_; }
  bool nextSequence(uint32_t& sequence, uint64_t& identity) override;
  bool append(const uint8_t* packet, uint64_t identity);
  bool takeAppendResult(bool& success);
  uint32_t count() const;
  static constexpr uint32_t capacity() { return storage_config::kCapacity; }
  bool oldest(Record& record) const { return readAfter(0, record); }
  bool newest(Record& record) const;
  bool lookup(uint64_t identity, Record& record) const;
  bool readAfter(uint64_t identity, Record& record) const;
  bool getOldestUndelivered(Record& record) const { return readAfter(state_.delivered_through, record); }
  bool getNextBacklog(Record& record) const;
  uint32_t backlogCount() const;
  // Reserved for future explicit BASE confirmation; TX_DONE never calls these.
  bool markDeliveredThrough(uint64_t identity);
  bool saveReplayCursor(uint64_t identity);
  uint64_t deliveredThrough() const { return state_.delivered_through; }
  uint64_t replayCursor() const { return state_.replay_cursor; }
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  bool appendIdle() const { return ready_ && !busy() && !append_result_ready_; }
  enum class Job { kNone, kNewPage, kReserve, kState, kAppend };
  enum class Phase { kErase, kHeader, kBlob };
  // M7P3: writeBlob() sub-steps, so a kPending flash result (SoftDevice
  // enabled) can be resumed on a later poll() pass instead of restarting the
  // blob write. Body-then-commit-word-last order is unchanged; kVerify is a
  // plain synchronous read, never asynchronous.
  enum class BlobStep { kBody, kCommit, kVerify };
  struct Page {
    uint64_t generation=0;
    uint8_t valid[13]{};
    uint8_t records_used=0, records_valid=0, sequence_used=0, state_used=0;
  };
  bool recover();
  bool pageValid(unsigned page) const { return pages_[page].generation != 0; }
  bool readSlot(unsigned page, unsigned slot, Record& record) const;
  bool startNewPage(bool append_after);
  bool startReservation();
  bool startState(journal_format::State next);
  void startBlob(uint32_t offset, const uint8_t* bytes, uint32_t size);
  FlashOpResult writeBlob();
  void finishBlob();
  void fail(bool append_failure);
  FlashBackend& flash_;
  uint64_t device_id_=0, sequence_end_=0, next_ticket_=0, newest_generation_=0;
  journal_format::State state_{};
  Diagnostics diagnostics_{};
  Page pages_[storage_config::kPageCount]{};
  int active_page_=-1;
  uint32_t target_page_=0, target_slot_=0, target_sequence_slot_=0, target_state_slot_=0;
  uint64_t target_generation_=0, pending_sequence_end_=0;
  bool append_after_new_page_=false, state_after_new_page_=false;
  bool append_result_ready_=false, append_success_=false, ready_=false;
  Job job_=Job::kNone;
  Phase phase_=Phase::kBlob;
  Record pending_record_{};
  journal_format::State pending_state_{};
  uint8_t blob_[storage_config::kPageHeaderSize]{};
  uint32_t blob_offset_=0, blob_size_=0;
  BlobStep blob_step_=BlobStep::kBody;
  // Shared by the erase branch and writeBlob(): at most one physical flash
  // primitive is ever in flight through this store at a time, so one flag
  // unambiguously means "the most recent program()/erasePage() call
  // returned kPending; call pollPending() next, do not resubmit."
  bool flash_op_awaiting_completion_=false;
};
}  // namespace orun_tlp
