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
  sequence_end_ = next_ticket_ = newest_generation_ = 0; active_page_ = -1; state_ = {};
  append_after_new_page_ = state_after_new_page_ = false;
  append_result_ready_ = append_success_ = false;
  if (!flash_.begin() || !recover()) return false;
  ready_ = true;
  if (active_page_ < 0) return startNewPage(false);
  // A reboot never reuses unused tickets. Persist the next block first.
  next_ticket_ = sequence_end_;
  return startReservation();
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

uint32_t HistoryStore::count() const { uint32_t n=0; for(unsigned p=0;p<kPageCount;++p)n+=pages_[p].records_valid; return n; }
bool HistoryStore::readSlot(unsigned p,unsigned s,Record& r) const { uint8_t b[kRecordSize]; return flash_.read(recordOffset(p,s),b,sizeof(b))&&decodeRecord(b,device_id_,r); }
bool HistoryStore::readAfter(uint64_t id,Record& out) const { bool found=false; for(unsigned p=0;p<kPageCount;++p)for(unsigned s=0;s<kRecordsPerPage;++s)if(bitSet(pages_[p].valid,s)){Record r;if(readSlot(p,s,r)&&r.identity>id&&(!found||r.identity<out.identity)){out=r;found=true;}}return found; }
bool HistoryStore::newest(Record& out) const { bool found=false; for(unsigned p=0;p<kPageCount;++p)for(unsigned s=0;s<kRecordsPerPage;++s)if(bitSet(pages_[p].valid,s)){Record r;if(readSlot(p,s,r)&&(!found||r.identity>out.identity)){out=r;found=true;}}return found; }
bool HistoryStore::lookup(uint64_t id,Record& out) const { return id && readAfter(id-1,out) && out.identity==id; }
bool HistoryStore::getNextBacklog(Record& r) const { return readAfter(state_.replay_cursor>state_.delivered_through?state_.replay_cursor:state_.delivered_through,r); }
uint32_t HistoryStore::backlogCount() const { uint32_t n=0;for(unsigned p=0;p<kPageCount;++p)for(unsigned s=0;s<kRecordsPerPage;++s)if(bitSet(pages_[p].valid,s)){Record r;if(readSlot(p,s,r)&&r.identity>state_.delivered_through)++n;}return n; }

bool HistoryStore::nextSequence(uint32_t& sequence,uint64_t& identity) { if(!canAppend())return false; sequence=uint32_t(next_ticket_); identity=++next_ticket_; return true; }
bool HistoryStore::append(const uint8_t* packet,uint64_t identity) {
  Record latest{};
  if(!appendIdle()||!packet||identity>next_ticket_||!validPacket(packet,identity,device_id_)||(newest(latest)&&identity<=latest.identity)){++diagnostics_.append_failures;return false;}
  pending_record_.identity=identity;memcpy(pending_record_.packet,packet,sizeof(pending_record_.packet));
  if(pages_[active_page_].records_used>=kRecordsPerPage){append_after_new_page_=true;if(startNewPage(true))return true;append_after_new_page_=false;++diagnostics_.append_failures;return false;}
  target_page_=active_page_;target_slot_=pages_[active_page_].records_used;uint8_t b[kRecordSize];encodeRecord(pending_record_,b);startBlob(recordOffset(target_page_,target_slot_),b,sizeof(b));job_=Job::kAppend;phase_=Phase::kBlob;return true;
}
bool HistoryStore::takeAppendResult(bool& ok){if(!append_result_ready_)return false;ok=append_success_;append_result_ready_=false;return true;}
bool HistoryStore::markDeliveredThrough(uint64_t id){Record r;if(!ready_||busy()||id<state_.delivered_through||!lookup(id,r))return false;if(id==state_.delivered_through)return true;State n=state_;n.delivered_through=id;return startState(n);}
bool HistoryStore::saveReplayCursor(uint64_t id){Record r;if(!ready_||busy()||(id&&!lookup(id,r)))return false;if(id==state_.replay_cursor)return true;State n=state_;n.replay_cursor=id;return startState(n);}

bool HistoryStore::startNewPage(bool append_after) {
  if (!ready_ || busy()) return false;
  target_page_ = active_page_ < 0 ? 0 : (unsigned(active_page_) + 1) % kPageCount;
  target_generation_ = newest_generation_ + 1; if (!target_generation_) return false;
  append_after_new_page_ = append_after; job_=Job::kNewPage; phase_=Phase::kErase; return true;
}
bool HistoryStore::startReservation() {
  if (!ready_ || busy() || active_page_ < 0 || sequence_end_ > UINT64_MAX-kSequenceBlockSize) return false;
  if (pages_[active_page_].sequence_used >= kSequenceSlotsPerPage) return startNewPage(false);
  target_page_=active_page_;target_sequence_slot_=pages_[active_page_].sequence_used;pending_sequence_end_=sequence_end_+kSequenceBlockSize;
  uint8_t b[kSequenceSlotSize];encodeSequenceEnd(pending_sequence_end_,b);startBlob(sequenceOffset(target_page_,target_sequence_slot_),b,sizeof(b));job_=Job::kReserve;phase_=Phase::kBlob;return true;
}
bool HistoryStore::startState(State next) {
  if(!ready_||busy()||active_page_<0||state_.generation==UINT64_MAX)return false;
  if(pages_[active_page_].state_used>=kStateSlotsPerPage){ pending_state_=next; state_after_new_page_=true; return startNewPage(false); }
  next.generation=state_.generation+1;pending_state_=next;target_page_=active_page_;target_state_slot_=pages_[active_page_].state_used;
  uint8_t b[kStateSlotSize];encodeState(next,b);startBlob(stateOffset(target_page_,target_state_slot_),b,sizeof(b));job_=Job::kState;phase_=Phase::kBlob;return true;
}
void HistoryStore::startBlob(uint32_t off,const uint8_t* b,uint32_t n){memcpy(blob_,b,n);blob_offset_=off;blob_size_=n;}
bool HistoryStore::writeBlob(){const bool append_failure=job_==Job::kAppend||append_after_new_page_;if(!flash_.program(blob_offset_,blob_,blob_size_-4)||!flash_.program(blob_offset_+blob_size_-4,blob_+blob_size_-4,4)){fail(append_failure);return false;}uint8_t verify[kPageHeaderSize];if(!flash_.read(blob_offset_,verify,blob_size_)||memcmp(verify,blob_,blob_size_)){fail(append_failure);return false;}return true;}
void HistoryStore::fail(bool append_failure){if(append_failure){++diagnostics_.append_failures;append_result_ready_=true;append_success_=false;}else ++diagnostics_.metadata_failures;append_after_new_page_=false;state_after_new_page_=false;job_=Job::kNone;ready_=false;}
void HistoryStore::finishBlob(){
  if(job_==Job::kNewPage){pages_[target_page_]={};pages_[target_page_].generation=target_generation_;newest_generation_=target_generation_;active_page_=target_page_;job_=Job::kNone;if(!startReservation())fail(append_after_new_page_);return;}
  if(job_==Job::kReserve){++pages_[target_page_].sequence_used;sequence_end_=pending_sequence_end_;job_=Job::kNone;if(state_after_new_page_){state_after_new_page_=false;if(!startState(pending_state_))fail(false);return;}if(append_after_new_page_){append_after_new_page_=false;if(!append(pending_record_.packet,pending_record_.identity)){append_result_ready_=true;append_success_=false;ready_=false;}}return;}
  if(job_==Job::kState){++pages_[target_page_].state_used;state_=pending_state_;job_=Job::kNone;return;}
  pages_[target_page_].records_used=target_slot_+1;pages_[target_page_].valid[target_slot_/8]|=1U<<(target_slot_%8);++pages_[target_page_].records_valid;++diagnostics_.appended;job_=Job::kNone;append_result_ready_=true;append_success_=true;
}
void HistoryStore::poll(){
  if(!ready_)return;
  if(job_==Job::kNone){if(!append_result_ready_&&next_ticket_==sequence_end_&&!startReservation())fail(false);return;}
  if(phase_==Phase::kErase){const uint32_t old=pages_[target_page_].records_valid;if(!flash_.erasePage(target_page_)){fail(append_after_new_page_);return;}diagnostics_.overwritten+=old;phase_=Phase::kHeader;uint8_t b[kStaticHeaderSize];encodePage(target_generation_,device_id_,b);startBlob(pageOffset(target_page_),b,sizeof(b));return;}
  if (!writeBlob()) return;
  finishBlob();
}
}  // namespace orun_tlp
