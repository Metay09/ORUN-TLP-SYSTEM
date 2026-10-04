#include "history_store.h"
#include <string.h>

namespace orun_tlp {
using namespace storage_config;
using namespace journal_format;
namespace {
uint32_t pageOffset(unsigned p) { return p * kPageSize; }
uint32_t recordOffset(unsigned p, unsigned s) { return pageOffset(p) + kPageHeaderSize + s * kRecordSize; }
uint32_t sequenceOffset(unsigned p, unsigned s) { return pageOffset(p) + kStaticHeaderSize + s * kSequenceSlotSize; }
uint32_t stateOffset(unsigned p, unsigned s) { return pageOffset(p) + kStaticHeaderSize + kSequenceSlotsPerPage * kSequenceSlotSize + s * kStateSlotSize; }
bool bitSet(const uint8_t* p, unsigned n) { return p[n / 8] & (1U << (n % 8)); }
}

bool HistoryStore::begin(uint64_t device) {
  ready_ = false; job_ = Job::kNone; diagnostics_ = {}; device_id_ = device;
  sequence_end_ = next_ticket_ = newest_generation_ = 0;
  acknowledged_through_ = 0;
  active_page_ = -1;
  state_ = {};
  append_after_new_page_ = false;
  pending_capacity_lost_undelivered_ = 0;
  append_result_ready_ = append_success_ = false;
  blob_step_ = BlobStep::kBody; flash_op_awaiting_completion_ = false;
  if (!flash_.begin() || !recover()) return false;
  acknowledged_through_ = state_.delivered_through;
  ready_ = true;
  if (active_page_ < 0) return startNewPage(false);
  // A reboot never reuses unused tickets, but recovery itself must stay
  // read-only. Skip the previously reserved-but-unused range in RAM and wait
  // until an application owner actually needs a new ticket block.
  next_ticket_ = sequence_end_;
  return true;
}

bool HistoryStore::recover() {
  bool old_format = false;
  for (unsigned p=0;p<kPageCount;++p) {
    pages_[p] = {};
    uint8_t header[kStaticHeaderSize];
    if (!flash_.read(pageOffset(p),header,sizeof(header))) return false;
    old_format |= get32(header) == kPageMagic && header[4] == 2;
    uint64_t gen = 0;
    if (!decodePage(header,device_id_,gen)) {
      if (!erased(header,sizeof(header)) && looksLikeJournalHeader(header)) ++diagnostics_.recovery_corruptions;
      continue;
    }
    pages_[p].generation = gen;
    if (gen > newest_generation_) { newest_generation_ = gen; active_page_ = p; }
    for (unsigned s=0;s<kSequenceSlotsPerPage;++s) {
      uint8_t bytes[kSequenceSlotSize]; uint64_t end=0;
      if (!flash_.read(sequenceOffset(p,s),bytes,sizeof(bytes))) return false;
      if (erased(bytes,sizeof(bytes))) break;
      pages_[p].sequence_used = s + 1;
      if (!decodeSequenceEnd(bytes,end)) { ++diagnostics_.recovery_corruptions; continue; }
      if (end > sequence_end_) sequence_end_ = end;
    }
    for (unsigned s=0;s<kStateSlotsPerPage;++s) {
      uint8_t bytes[kStateSlotSize]; State candidate{};
      if (!flash_.read(stateOffset(p,s),bytes,sizeof(bytes))) return false;
      if (erased(bytes,sizeof(bytes))) break;
      pages_[p].state_used = s + 1;
      if (!decodeState(bytes,candidate)) { ++diagnostics_.recovery_corruptions; continue; }
      if (candidate.generation > state_.generation) state_ = candidate;
    }
    uint64_t previous = 0;
    for (unsigned s=0;s<kRecordsPerPage;++s) {
      uint8_t bytes[kRecordSize]; Record r{};
      if (!flash_.read(recordOffset(p,s),bytes,sizeof(bytes))) return false;
      if (erased(bytes,sizeof(bytes))) continue;
      pages_[p].records_used = s + 1;
      if (!decodeRecord(bytes,device_id_,r) || (previous && r.identity <= previous)) { ++diagnostics_.recovery_corruptions; continue; }
      previous = r.identity; pages_[p].valid[s/8] |= 1U << (s%8); ++pages_[p].records_valid;
    }
  }
  // Never interpret development v2 identities as v3. A v2-only partition
  // requires an explicit development reset; no automatic migration or erase.
  if (active_page_ < 0 && old_format) return false;
  // This partition is exclusively ORUN-owned. With no valid page, including
  // bit-partial first magic/CRC/commit, begin() reinitializes page zero only.
  // If any v3 page survived, keep its history and recover normally instead.
  return true;
}

bool HistoryStore::erasePending() const {
  return job_ == Job::kNewPage && phase_ == Phase::kErase;
}

uint32_t HistoryStore::count() const { uint32_t n=0; for(unsigned p=0;p<kPageCount;++p)n+=pages_[p].records_valid; return n; }
bool HistoryStore::readSlot(unsigned p,unsigned s,Record& r) const { uint8_t b[kRecordSize]; return flash_.read(recordOffset(p,s),b,sizeof(b))&&decodeRecord(b,device_id_,r); }

bool HistoryStore::readNextRetainedStrict(uint64_t id, Record& out) const {
  bool found = false;
  for (unsigned p = 0; p < kPageCount; ++p) {
    for (unsigned s = 0; s < kRecordsPerPage; ++s) {
      if (!bitSet(pages_[p].valid, s)) continue;
      Record record{};
      // Receipt admission is fail-closed: a slot recovery classified as valid
      // may not be silently skipped merely because this read/decode fails.
      if (!readSlot(p, s, record)) return false;
      if (record.identity > id &&
          (!found || record.identity < out.identity)) {
        out = record;
        found = true;
      }
    }
  }
  return found;
}

uint32_t HistoryStore::countCapacityLostUndelivered(unsigned page) const {
  if (page >= kPageCount) return 0;
  const uint64_t confirmed_through =
      acknowledged_through_ > state_.delivered_through
          ? acknowledged_through_
          : state_.delivered_through;
  uint32_t lost = 0;
  for (unsigned slot = 0; slot < kRecordsPerPage; ++slot) {
    if (!bitSet(pages_[page].valid, slot)) continue;
    Record record{};
    // A previously valid record which can no longer be decoded immediately
    // before its page is erased is conservatively reported as capacity loss.
    if (!readSlot(page, slot, record) ||
        record.identity > confirmed_through)
      ++lost;
  }
  return lost;
}

bool HistoryStore::readAfter(uint64_t id,Record& out) const { bool found=false; for(unsigned p=0;p<kPageCount;++p)for(unsigned s=0;s<kRecordsPerPage;++s)if(bitSet(pages_[p].valid,s)){Record r;if(readSlot(p,s,r)&&r.identity>id&&(!found||r.identity<out.identity)){out=r;found=true;}}return found; }
bool HistoryStore::newest(Record& out) const { bool found=false; for(unsigned p=0;p<kPageCount;++p)for(unsigned s=0;s<kRecordsPerPage;++s)if(bitSet(pages_[p].valid,s)){Record r;if(readSlot(p,s,r)&&(!found||r.identity>out.identity)){out=r;found=true;}}return found; }
bool HistoryStore::lookup(uint64_t id,Record& out) const { return id && readAfter(id-1,out) && out.identity==id; }
bool HistoryStore::getNextBacklog(Record& r) const {
  // Persistent replay_cursor is legacy development state and is deliberately
  // not a production replay selector. Durable delivery is the only persistent
  // lower bound; later SF3 RAM scheduling may add a transient bound above it.
  return getOldestUndelivered(r);
}
uint32_t HistoryStore::backlogCount() const { uint32_t n=0;for(unsigned p=0;p<kPageCount;++p)for(unsigned s=0;s<kRecordsPerPage;++s)if(bitSet(pages_[p].valid,s)){Record r;if(readSlot(p,s,r)&&r.identity>state_.delivered_through)++n;}return n; }

bool HistoryStore::prepareAppend() {
  if (canAppend()) return true;
  if (!appendIdle() || next_ticket_ != sequence_end_) return false;
  if (!startReservation()) fail(false);
  return false;
}

bool HistoryStore::nextSequence(uint32_t& sequence,uint64_t& identity) {
  if(!canAppend()){
    if(appendIdle()&&next_ticket_==sequence_end_&&!startReservation())fail(false);
    return false;
  }
  sequence=uint32_t(next_ticket_);
  identity=++next_ticket_;
  return true;
}
bool HistoryStore::append(const uint8_t* packet,uint64_t identity) {
  Record latest{};
  if(!appendIdle()||!packet||identity>next_ticket_||!validPacket(packet,identity,device_id_)||(newest(latest)&&identity<=latest.identity)){++diagnostics_.append_failures;return false;}
  pending_record_.identity=identity;memcpy(pending_record_.packet,packet,sizeof(pending_record_.packet));
  if(pages_[active_page_].records_used>=kRecordsPerPage){append_after_new_page_=true;if(startNewPage(true))return true;append_after_new_page_=false;++diagnostics_.append_failures;return false;}
  target_page_=active_page_;target_slot_=pages_[active_page_].records_used;uint8_t b[kRecordSize];encodeRecord(pending_record_,b);startBlob(recordOffset(target_page_,target_slot_),b,sizeof(b));job_=Job::kAppend;phase_=Phase::kBlob;return true;
}
bool HistoryStore::takeAppendResult(bool& ok){if(!append_result_ready_)return false;ok=append_success_;append_result_ready_=false;return true;}
bool HistoryStore::acknowledgeDeliveredRecord(uint64_t id) {
  if (!ready_ || busy() || id == 0) return false;

  // Duplicate/late receipt for an already contiguous acknowledged prefix is
  // idempotent and cannot regress either RAM or durable progress.
  if (id <= acknowledged_through_) return true;

  Record expected{};
  if (!readNextRetainedStrict(acknowledged_through_, expected) ||
      expected.identity != id) {
    return false;
  }

  acknowledged_through_ = id;
  return true;
}

HistoryStore::DeliveryCheckpointResult
HistoryStore::checkpointAcknowledgedDelivery() {
  if (!ready_) return DeliveryCheckpointResult::kRejected;
  if (busy()) return DeliveryCheckpointResult::kBusy;
  if (acknowledged_through_ <= state_.delivered_through)
    return DeliveryCheckpointResult::kNoChange;

  if (active_page_ < 0)
    return DeliveryCheckpointResult::kRejected;

  // acknowledgeDeliveredRecord() validated this identity while it was an
  // actual retained record. A later capacity-driven page erase does not revoke
  // an already authenticated BACKEND_DURABLE fact, so checkpointing must not
  // deadlock merely because the acknowledged record has since left the ring.

  if (pages_[active_page_].state_used >= kStateSlotsPerPage) {
    ++diagnostics_.delivery_checkpoint_deferrals;
    return DeliveryCheckpointResult::kDeferredNoStateSlot;
  }

  State next = state_;
  next.delivered_through = acknowledged_through_;
  // Persistent replay_cursor is outside the production SF1/SF3 contract.
  // Any new trusted checkpoint clears legacy cursor influence.
  next.replay_cursor = 0;
  if (!startState(next)) return DeliveryCheckpointResult::kRejected;
  return DeliveryCheckpointResult::kStarted;
}

bool HistoryStore::markDeliveredThrough(uint64_t id) {
  // Preserve the legacy API's no-regression and false-without-new-RAM-progress
  // behavior. The new SF1 API owns explicit deferred RAM progress.
  if (!ready_ || busy() || active_page_ < 0 ||
      state_.generation == UINT64_MAX ||
      id < state_.delivered_through)
    return false;
  if (id == state_.delivered_through) return true;
  if (pages_[active_page_].state_used >= kStateSlotsPerPage) return false;

  if (acknowledged_through_ == state_.delivered_through) {
    if (!acknowledgeDeliveredRecord(id)) return false;
  } else if (id != acknowledged_through_) {
    return false;
  }

  const auto result = checkpointAcknowledgedDelivery();
  return result == DeliveryCheckpointResult::kStarted ||
         result == DeliveryCheckpointResult::kNoChange;
}

bool HistoryStore::saveReplayCursor(uint64_t id) {
  Record r;
  if (!ready_ || busy() || (id && !lookup(id, r))) return false;
  if (id == state_.replay_cursor) return true;
  // Legacy development API must not create metadata-only page rotation either.
  if (active_page_ < 0 ||
      pages_[active_page_].state_used >= kStateSlotsPerPage)
    return false;
  State n = state_;
  n.replay_cursor = id;
  return startState(n);
}

bool HistoryStore::startNewPage(bool append_after) {
  if (!ready_ || busy()) return false;
  target_page_ =
      active_page_ < 0 ? 0 : (unsigned(active_page_) + 1) % kPageCount;
  target_generation_ = newest_generation_ + 1;
  if (!target_generation_) return false;

  // Capture the capacity-loss classification while the target page is still
  // readable. Publish it only after the physical erase actually succeeds.
  pending_capacity_lost_undelivered_ =
      countCapacityLostUndelivered(target_page_);
  append_after_new_page_ = append_after;
  job_ = Job::kNewPage;
  phase_ = Phase::kErase;
  return true;
}
bool HistoryStore::startReservation() {
  if (!ready_ || busy() || active_page_ < 0 || sequence_end_ > UINT64_MAX-kSequenceBlockSize) return false;
  if (pages_[active_page_].sequence_used >= kSequenceSlotsPerPage) return startNewPage(false);
  target_page_=active_page_;target_sequence_slot_=pages_[active_page_].sequence_used;pending_sequence_end_=sequence_end_+kSequenceBlockSize;
  uint8_t b[kSequenceSlotSize];encodeSequenceEnd(pending_sequence_end_,b);startBlob(sequenceOffset(target_page_,target_sequence_slot_),b,sizeof(b));job_=Job::kReserve;phase_=Phase::kBlob;return true;
}
bool HistoryStore::startState(State next) {
  if (!ready_ || busy() || active_page_ < 0 ||
      state_.generation == UINT64_MAX)
    return false;
  // SF1 invariant: delivery/replay metadata may never rotate/erase a History
  // page. State durability waits for a normal record-driven page transition.
  if (pages_[active_page_].state_used >= kStateSlotsPerPage) return false;
  next.generation = state_.generation + 1;
  pending_state_ = next;
  target_page_ = active_page_;
  target_state_slot_ = pages_[active_page_].state_used;
  uint8_t b[kStateSlotSize];
  encodeState(next, b);
  startBlob(stateOffset(target_page_, target_state_slot_), b, sizeof(b));
  job_ = Job::kState;
  phase_ = Phase::kBlob;
  return true;
}
void HistoryStore::startBlob(uint32_t off,const uint8_t* b,uint32_t n){memcpy(blob_,b,n);blob_offset_=off;blob_size_=n;blob_step_=BlobStep::kBody;flash_op_awaiting_completion_=false;}
// M7P3: body/CRC then a separate final commit word, exactly as before -- the
// power-cut invariant is unchanged. The only difference from the pre-M7P3
// version is that each physical program() may now return kPending (only
// possible with SoftDevice enabled, never in today's shipped M0-M7P3
// runtime); this resumes the same step on the next poll() via
// pollPending() instead of restarting or skipping ahead. kVerify's read is
// always synchronous, so it is never affected by kPending.
FlashOpResult HistoryStore::writeBlob(){
  const bool append_failure=job_==Job::kAppend||append_after_new_page_;
  if(blob_step_==BlobStep::kBody){
    const FlashOpResult r=flash_op_awaiting_completion_?flash_.pollPending():flash_.program(blob_offset_,blob_,blob_size_-4);
    if(r==FlashOpResult::kPending){flash_op_awaiting_completion_=true;return FlashOpResult::kPending;}
    flash_op_awaiting_completion_=false;
    if(r==FlashOpResult::kFailed){fail(append_failure);return FlashOpResult::kFailed;}
    blob_step_=BlobStep::kCommit;
  }
  if(blob_step_==BlobStep::kCommit){
    const FlashOpResult r=flash_op_awaiting_completion_?flash_.pollPending():flash_.program(blob_offset_+blob_size_-4,blob_+blob_size_-4,4);
    if(r==FlashOpResult::kPending){flash_op_awaiting_completion_=true;return FlashOpResult::kPending;}
    flash_op_awaiting_completion_=false;
    if(r==FlashOpResult::kFailed){fail(append_failure);return FlashOpResult::kFailed;}
    blob_step_=BlobStep::kVerify;
  }
  uint8_t verify[kPageHeaderSize];
  if(!flash_.read(blob_offset_,verify,blob_size_)||memcmp(verify,blob_,blob_size_)){blob_step_=BlobStep::kBody;fail(append_failure);return FlashOpResult::kFailed;}
  blob_step_=BlobStep::kBody;
  return FlashOpResult::kDone;
}
void HistoryStore::fail(bool append_failure){if(append_failure){++diagnostics_.append_failures;append_result_ready_=true;append_success_=false;}else ++diagnostics_.metadata_failures;append_after_new_page_=false;pending_capacity_lost_undelivered_=0;blob_step_=BlobStep::kBody;flash_op_awaiting_completion_=false;job_=Job::kNone;ready_=false;}
void HistoryStore::finishBlob(){
  if(job_==Job::kNewPage){pages_[target_page_]={};pages_[target_page_].generation=target_generation_;newest_generation_=target_generation_;active_page_=target_page_;job_=Job::kNone;if(!startReservation())fail(append_after_new_page_);return;}
  if(job_==Job::kReserve){++pages_[target_page_].sequence_used;sequence_end_=pending_sequence_end_;job_=Job::kNone;if(append_after_new_page_){append_after_new_page_=false;if(!append(pending_record_.packet,pending_record_.identity)){append_result_ready_=true;append_success_=false;ready_=false;}}return;}
  if(job_==Job::kState){++pages_[target_page_].state_used;state_=pending_state_;job_=Job::kNone;return;}
  pages_[target_page_].records_used=target_slot_+1;pages_[target_page_].valid[target_slot_/8]|=1U<<(target_slot_%8);++pages_[target_page_].records_valid;++diagnostics_.appended;job_=Job::kNone;append_result_ready_=true;append_success_=true;
}
void HistoryStore::poll(){
  if(!ready_)return;
  // Idle recovery is intentionally read-only. Reservation is demand-driven by
  // prepareAppend()/nextSequence(), so reboot loops with no new work cannot
  // consume metadata slots or rotate/erase history pages.
  if(job_==Job::kNone)return;
  if(phase_==Phase::kErase){
    const FlashOpResult r=flash_op_awaiting_completion_?flash_.pollPending():flash_.erasePage(target_page_);
    if(r==FlashOpResult::kPending){flash_op_awaiting_completion_=true;return;}
    flash_op_awaiting_completion_=false;
    if(r==FlashOpResult::kFailed){fail(append_after_new_page_);return;}
    const uint32_t old=pages_[target_page_].records_valid;
    diagnostics_.overwritten += old;
    diagnostics_.capacity_lost_undelivered +=
        pending_capacity_lost_undelivered_;
    pending_capacity_lost_undelivered_ = 0;
    phase_=Phase::kHeader;
    uint8_t b[kStaticHeaderSize];encodePage(target_generation_,device_id_,b);startBlob(pageOffset(target_page_),b,sizeof(b));
    return;
  }
  if(writeBlob()!=FlashOpResult::kDone)return;
  finishBlob();
}
}  // namespace orun_tlp
